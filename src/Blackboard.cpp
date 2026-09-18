#include <agentic_blackboard/Blackboard.hpp>
#include <agentic_blackboard/Orchestrator.hpp>
#include <agentic_blackboard/DeltaEngine.hpp>
#include <agentic_blackboard/Monitor.hpp>
#include "buffer.hpp"
#include "json.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include "engine/store.hpp"
#include "engine/credential_manager.hpp"
#include <algorithm>
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <nlohmann/json.hpp>

namespace {

std::string hash_token_sha256(const std::string& token) {
    const std::string salt = "ab_salt_token_v1:";
    std::string salted = salt + token;
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, salted.data(), salted.size());
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

std::string extract_uuid_from_buf(const lite3cpp::Buffer& buf) {
    if (buf.size() == 0) return "";
    try {
        if (buf.get_type(0, "header") == lite3cpp::Type::Object) {
            size_t h_idx = buf.get_obj(0, "header");
            if (buf.get_type(h_idx, "uuid") == lite3cpp::Type::String) {
                std::string u = std::string(buf.get_str(h_idx, "uuid"));
                if (!u.empty()) return u;
            }
        }
    } catch (...) {}
    try {
        if (buf.get_type(0, "id") == lite3cpp::Type::String) {
            std::string u = std::string(buf.get_str(0, "id"));
            if (!u.empty()) return u;
        }
    } catch (...) {}
    try {
        if (buf.get_type(0, "project_id") == lite3cpp::Type::String) {
            std::string u = std::string(buf.get_str(0, "project_id"));
            if (!u.empty()) return u;
        }
    } catch (...) {}
    try {
        if (buf.get_type(0, "_binary") == lite3cpp::Type::Bytes) {
            auto bin = buf.get_bytes(0, "_binary");
            std::vector<uint8_t> vec;
            vec.reserve(bin.size());
            for (auto b : bin) vec.push_back(static_cast<uint8_t>(b));
            lite3cpp::Buffer nested(std::move(vec));
            return extract_uuid_from_buf(nested);
        }
    } catch (...) {}
    return "";
}

std::string resolve_node_uuid(l3kvg::Engine* engine, uint64_t nid, uint32_t principal_id = 0) {
    try {
        std::string nkey = std::string(l3kvg::KeyBuilder::node_key(nid));
        uint32_t eff_principal = (principal_id == 0) ? l3kv::ADMIN_UID : principal_id;
        auto buf = engine->get_store()->get(nkey, eff_principal);
        std::string u = extract_uuid_from_buf(buf);
        if (!u.empty()) {
            return u;
        }
    } catch (...) {}

    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(nid));
    return std::string(hex);
}

} // anonymous namespace

namespace agentic_blackboard {

Blackboard::Blackboard(const std::string& db_path, uint32_t node_id) {
    engine_ = std::make_unique<l3kvg::Engine>(db_path, node_id);
    try {
        auto* store = engine_->get_store();
        lite3cpp::Buffer buf = store->get("auth:registry:users", l3kv::ADMIN_UID);
        if (buf.size() > 0) {
            std::string js;
            try {
                js = lite3cpp::lite3_json::to_json_string(buf, 0);
            } catch (...) {}
            if (js.empty()) {
                try {
                    js = std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
                } catch (...) {}
            }
            if (!js.empty()) {
                auto j = nlohmann::json::parse(js);
                auto user_list = j.is_array() ? j : (j.contains("users") && j["users"].is_array() ? j["users"] : nlohmann::json::array());
                std::unique_lock lock(auth_mutex_);
                for (const auto& item : user_list) {
                    std::string u = item.value("username", "");
                    std::string r = item.value("role", "");
                    if (!u.empty()) {
                        bool exists = false;
                        for (const auto& entry : registered_users_) {
                            if (entry.first == u) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            registered_users_.emplace_back(u, r);
                        }
                        register_user_credentials(u, u + "-key");
                        if (r == "admin" || u == "admin") {
                            store->credentials().set_acl(get_user_uid(u), "*", l3kv::Permission::READ | l3kv::Permission::WRITE | l3kv::Permission::ADMIN);
                        }
                    }
                }
            }
        }
    } catch (...) {}
}

Blackboard::~Blackboard() = default;

void Blackboard::set_auth_mode(const std::string& mode) {
    if (mode != "token" && mode != "trusted_network") {
        return;
    }
    std::unique_lock lock(auth_mutex_);
    auth_mode_ = mode;
}

std::string Blackboard::get_auth_mode() const {
    std::shared_lock lock(auth_mutex_);
    return auth_mode_;
}

bool Blackboard::register_token(const std::string& token, const std::string& user, const std::string& role) {
    if (token.empty() || user.empty()) return false;

    // Ensure user credentials exist in L3KV credential manager so get_user_uid(user) works
    register_user_credentials(user, user + "-key");
    uint32_t uid = get_user_uid(user);
    auto* store = engine_->get_store();
    if (role == "admin" || user == "admin") {
        store->credentials().set_acl(uid, "*", l3kv::Permission::READ | l3kv::Permission::WRITE | l3kv::Permission::ADMIN);
    }

    // Salted SHA-256 token hashing
    std::string token_hash = hash_token_sha256(token);
    if (token_hash.empty()) return false;
    std::string db_key = "auth:token:" + token_hash;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    nlohmann::json meta = {
        {"username", user},
        {"role", role},
        {"created_at", now}
    };

    store->put(db_key, meta.dump());
    store->wait_all_shards();

    {
        std::unique_lock lock(auth_mutex_);
        bool found = false;
        for (auto& entry : registered_users_) {
            if (entry.first == user) {
                entry.second = role;
                found = true;
                break;
            }
        }
        if (!found) {
            registered_users_.emplace_back(user, role);
        }

        nlohmann::json user_arr = nlohmann::json::array();
        for (const auto& entry : registered_users_) {
            user_arr.push_back({
                {"username", entry.first},
                {"role", entry.second}
            });
        }
        store->put("auth:registry:users", user_arr.dump());
        store->wait_all_shards();
    }

    return true;
}

std::vector<std::pair<std::string, std::string>> Blackboard::get_registered_users() const {
    std::shared_lock lock(auth_mutex_);
    return registered_users_;
}

bool Blackboard::validate_token(const std::string& token, std::string& out_user, std::string& out_role) {
    nlohmann::json meta;
    return validate_token(token, out_user, out_role, meta);
}

bool Blackboard::validate_token(const std::string& token, std::string& out_user, std::string& out_role, nlohmann::json& out_meta) {
    if (token.empty()) return false;

    std::string token_hash = hash_token_sha256(token);
    if (token_hash.empty()) return false;
    std::string db_key = "auth:token:" + token_hash;

    auto* store = engine_->get_store();
    lite3cpp::Buffer buf = store->get(db_key);
    if (buf.size() == 0) return false;

    nlohmann::json parsed_json;

    // First try converting lite3cpp Buffer to JSON
    try {
        std::string js = lite3cpp::lite3_json::to_json_string(buf, 0);
        if (!js.empty()) {
            parsed_json = nlohmann::json::parse(js);
        }
    } catch (...) {}

    // Fallback: raw buffer as JSON string
    if (parsed_json.is_null() || !parsed_json.is_object()) {
        try {
            std::string raw(reinterpret_cast<const char*>(buf.data()), buf.size());
            if (!raw.empty()) {
                parsed_json = nlohmann::json::parse(raw);
            }
        } catch (...) {}
    }

    std::string u;
    std::string r;
    if (!parsed_json.is_null() && parsed_json.is_object()) {
        u = parsed_json.value("username", "");
        r = parsed_json.value("role", "");
        out_meta = parsed_json;
    } else {
        try {
            u = std::string(buf.get_str(0, "username"));
            r = std::string(buf.get_str(0, "role"));
        } catch (...) {}
        if (!u.empty()) {
            out_meta = {
                {"username", u},
                {"role", r}
            };
            try { out_meta["agent"] = std::string(buf.get_str(0, "agent")); } catch (...) {}
            try { out_meta["surface_id"] = std::string(buf.get_str(0, "surface_id")); } catch (...) {}
            try { out_meta["surface_type"] = std::string(buf.get_str(0, "surface_type")); } catch (...) {}
            try { out_meta["context_id"] = std::string(buf.get_str(0, "context_id")); } catch (...) {}
        }
    }

    if (u.empty()) return false;

    out_user = u;
    out_role = r;
    return true;
}

bool Blackboard::register_user_credentials(const std::string& username, const std::string& public_key) {
    uint32_t uid = get_user_uid(username);
    auto* store = engine_->get_store();
    
    // Register the user identity
    store->credentials().register_user(uid, username, public_key);
    
    // Grant baseline read/write access to system keys if needed (optional)
    return true;
}

uint32_t Blackboard::get_user_uid(const std::string& username) const {
    // Deterministic simple hash mapping string to uint32_t
    uint32_t hash = 5381;
    for (char c : username) {
        hash = ((hash << 5) + hash) + c;
    }
    if (hash == 0 || hash == 0xFFFFFFFF) hash = 1;
    return hash;
}

bool Blackboard::commit_cpb_entry(const CpbEntry& entry, uint32_t principal_id) {
    CpbEntry adjusted = entry;
    std::cout << "[Blackboard] commit_cpb_entry called for statement: " << adjusted.payload.statement << std::endl;

    // ORPHAN PREVENTION: Every atom must be anchored to at least (user_id or agent_id) AND a Project
    if ((adjusted.header.origin.user_id.empty() && adjusted.header.origin.agent_id.empty()) || adjusted.header.origin.project_id.empty()) {
        std::cerr << "[Blackboard] Rejecting Atom " << (adjusted.header.uuid.empty() ? "UNNAMED" : adjusted.header.uuid)
                  << ": Missing mandatory anchors (User/Agent AND Project required)." << std::endl;
        return false;
    }

    // UUID GENERATION: Ensure we have a unique key for the substrate
    if (adjusted.header.uuid.empty()) {
        // Deterministic hash-based ID for idempotency within the same session/payload
        uint32_t h = std::hash<std::string>{}(adjusted.payload.statement);
        char hex[9];
        sprintf(hex, "%08x", h);
        adjusted.header.uuid = "atom-" + std::string(hex);
    }

    // Extract agent name from author ID
    std::string author_agent = adjusted.header.origin.agent_id;
    std::string author_name = author_agent;
    if (author_agent.starts_with("identity:")) {
        author_name = author_agent.substr(9);
    } else if (author_agent.starts_with("agent:")) {
        author_name = author_agent.substr(6);
    } else if (author_agent.starts_with("user:")) {
        author_name = author_agent.substr(5);
    }

    std::string user_id = adjusted.header.origin.user_id;
    std::string user_name = user_id;
    if (user_id.starts_with("identity:")) {
        user_name = user_id.substr(9);
    } else if (user_id.starts_with("user:")) {
        user_name = user_id.substr(5);
    } else if (user_id.starts_with("agent:")) {
        user_name = user_id.substr(6);
    }

    // Security check 1: Ensure user is not impersonating someone else
    if (principal_id != 0) {
        bool authorized = false;
        if (!user_name.empty()) {
            if (get_user_uid(user_name) == principal_id || (!user_id.empty() && get_user_uid(user_id) == principal_id) ||
                get_user_uid("user:" + user_name) == principal_id) {
                authorized = true;
            }
        }
        if (!authorized && !author_name.empty()) {
            if (get_user_uid(author_name) == principal_id || (!author_agent.empty() && get_user_uid(author_agent) == principal_id) ||
                get_user_uid("agent:" + author_name) == principal_id || get_user_uid("user:" + author_name) == principal_id) {
                authorized = true;
            }
            if (!authorized && author_name.ends_with("-agent")) {
                std::string base = author_name.substr(0, author_name.length() - 6);
                if (get_user_uid(base) == principal_id || get_user_uid("user:" + base) == principal_id) {
                    authorized = true;
                }
            }
        }

        if (!authorized) {
            std::cerr << "[Blackboard] Rejecting Atom " << adjusted.header.uuid 
                      << ": Access Denied (Principal ID " << principal_id 
                      << " cannot write on behalf of user " << user_id 
                      << " or agent " << author_agent << ")" << std::endl;
            return false;
        }
    }

    auto key_uuid = engine_->get_resolver().parse_uuid(adjusted.header.uuid);
    std::string db_key = std::string(l3kvg::KeyBuilder::node_key(key_uuid));
    auto& creds = engine_->get_store()->credentials();

    // Check if node exists
    bool exists = false;
    try {
        auto node = engine_->get_node(adjusted.header.uuid);
        if (node && node->has_attribute("header")) {
            exists = true;
        }
    } catch (...) {
        // Doesn't exist
    }

    if (exists) {
        // Security check 2: Check if user has WRITE access on this existing key
        auto perm = creds.check_permission(principal_id, db_key);
        bool has_write = (perm & l3kv::Permission::WRITE) || (perm & l3kv::Permission::ADMIN);
        if (!has_write && principal_id != 0) {
            // Check if principal matches stored author identity across restarts
            try {
                auto buf = engine_->get_store()->get(db_key, l3kv::ADMIN_UID);
                if (buf.size() > 0) {
                    CpbEntry existing = CpbEntry::deserialize(buf);
                    const auto& exist_user = existing.header.origin.user_id;
                    const auto& exist_agent = existing.header.origin.agent_id;

                    std::string exist_user_name = exist_user;
                    if (exist_user.starts_with("identity:")) exist_user_name = exist_user.substr(9);
                    else if (exist_user.starts_with("user:")) exist_user_name = exist_user.substr(5);
                    else if (exist_user.starts_with("agent:")) exist_user_name = exist_user.substr(6);

                    std::string exist_agent_name = exist_agent;
                    if (exist_agent.starts_with("identity:")) exist_agent_name = exist_agent.substr(9);
                    else if (exist_agent.starts_with("agent:")) exist_agent_name = exist_agent.substr(6);
                    else if (exist_agent.starts_with("user:")) exist_agent_name = exist_agent.substr(5);

                    if ((!exist_user.empty() && (get_user_uid(exist_user) == principal_id || get_user_uid(exist_user_name) == principal_id || get_user_uid("user:" + exist_user_name) == principal_id)) ||
                        (!exist_agent.empty() && (get_user_uid(exist_agent) == principal_id || get_user_uid(exist_agent_name) == principal_id || get_user_uid("agent:" + exist_agent_name) == principal_id || get_user_uid("user:" + exist_agent_name) == principal_id))) {
                        creds.set_acl(principal_id, db_key, l3kv::Permission::READ | l3kv::Permission::WRITE);
                        std::string hex_part = db_key.substr(2);
                        creds.set_acl(principal_id, "e:out:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                        creds.set_acl(principal_id, "e:in:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                        has_write = true;
                    }
                    if (!has_write && !exist_agent_name.empty() && exist_agent_name.ends_with("-agent")) {
                        std::string base = exist_agent_name.substr(0, exist_agent_name.length() - 6);
                        if (get_user_uid(base) == principal_id || get_user_uid("user:" + base) == principal_id) {
                            creds.set_acl(principal_id, db_key, l3kv::Permission::READ | l3kv::Permission::WRITE);
                            std::string hex_part = db_key.substr(2);
                            creds.set_acl(principal_id, "e:out:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                            creds.set_acl(principal_id, "e:in:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                            has_write = true;
                        }
                    }
                }
            } catch (...) {}
        }

        if (!has_write) {
            std::cerr << "[Blackboard] Rejecting Atom " << adjusted.header.uuid 
                      << ": Access Denied (No WRITE permission for principal " << principal_id << ")" << std::endl;
            return false;
        }
    } else {
        // New node: Authorize the owner automatically
        if (principal_id != 0) {
            creds.set_acl(principal_id, db_key, l3kv::Permission::READ | l3kv::Permission::WRITE);
            // Authorize edges related to this node too
            std::string hex_part = db_key.substr(2); // "{hex_id}"
            creds.set_acl(principal_id, "e:out:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
            creds.set_acl(principal_id, "e:in:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
        }
    }

    // SAFE-MODE LOGIC: If isolated, mark as uncertain
    if (Orchestrator::instance().current_state() == Orchestrator::State::ISOLATED) {
        adjusted.taxonomy.uncertainty = true;
    }

    lite3cpp::Buffer buf;
    adjusted.serialize(buf);
    
    // FORCED LOCAL STORAGE: Ensure all atoms are stored on local substrate for shake-down
    std::cout << "[Blackboard] Committing Atom: " << adjusted.header.uuid << " (Project: " << adjusted.header.origin.project_id << ")" << std::endl;
    
    // Use the engine's put_node to ensure proper distributed routing, caching, and indexing
    engine_->put_node(adjusted.header.uuid, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));

    
    // IDENTITY-CENTRIC ENFORCEMENT: Ensure Links (No Orphans)
    const auto& origin_user_id = adjusted.header.origin.user_id;
    const auto& origin_agent_id = adjusted.header.origin.agent_id;
    auto project_id = adjusted.header.origin.project_id;

    // 1. Link Author (Identity)
    if (!origin_user_id.empty()) {
        auto user_node = engine_->get_node(origin_user_id);
        if (!user_node || !user_node->has_attribute("header")) {
            IdentityNode stub = {origin_user_id, "Unknown User (" + origin_user_id + ")", "USER", ""};
            commit_identity_node(stub);
        }
        engine_->add_edge(adjusted.header.uuid, rel::CREATED_BY, 1.0, origin_user_id);

        if (!origin_agent_id.empty() && origin_agent_id != origin_user_id) {
            auto agent_node = engine_->get_node(origin_agent_id);
            if (!agent_node || !agent_node->has_attribute("header")) {
                IdentityNode stub = {origin_agent_id, "Unknown Agent (" + origin_agent_id + ")", "AGENT", ""};
                commit_identity_node(stub);
            }
            engine_->add_edge(adjusted.header.uuid, rel::CREATED_BY, 1.0, origin_agent_id);
        }
    } else if (!origin_agent_id.empty()) {
        auto agent_node = engine_->get_node(origin_agent_id);
        if (!agent_node || !agent_node->has_attribute("header")) {
            IdentityNode stub = {origin_agent_id, "Unknown Agent (" + origin_agent_id + ")", "AGENT", ""};
            commit_identity_node(stub);
        }
        engine_->add_edge(adjusted.header.uuid, rel::CREATED_BY, 1.0, origin_agent_id);
    }

    // 2. Link Project
    if (!project_id.empty()) {
        auto project_node = engine_->get_node(project_id);
        if (!project_node->has_attribute("header")) {
            ProjectNode stub = {project_id, "Auto-created stub for " + project_id, "STUB"};
            commit_project_node(stub);
        }
        engine_->add_edge(adjusted.header.uuid, rel::BELONGS_TO, 1.0, project_id);
    }

    // 3. Propagate Edge ACLs for Source Node (Multi-Tenancy)
    if (principal_id != 0) {
        std::string hex_part = db_key.substr(2);
        creds.set_acl(principal_id, "e:out:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
        creds.set_acl(principal_id, "e:in:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
    }

    // 4. Auto-project Note Links
    for (const auto& link : adjusted.payload.note_links) {
        if (!link.target_uuid.empty()) {
            std::string rel_label = link.relation.empty() ? rel::SEE_ALSO : link.relation;
            engine_->add_edge(adjusted.header.uuid, rel_label, 1.0, link.target_uuid);
        }
    }

    // 5. Auto-project Reference Citations
    for (const auto& ref : adjusted.payload.references) {
        if (!ref.uuid.empty()) {
            engine_->add_edge(adjusted.header.uuid, rel::CITES, 1.0, ref.uuid);
        }
    }


    // Telemetry
    Monitor::instance().report_atom_commit();

    // Distributed Mirroring (ZeroMQ)
    Orchestrator::instance().broadcast_atom(adjusted);

    return true;
}



bool Blackboard::semantic_merge(const CpbEntry& incoming) {
    auto* store = engine_->get_store();
    std::string key(l3kvg::KeyBuilder::node_key(engine_->get_resolver().parse_uuid(incoming.header.uuid)));
    
    // Check if we have a local version
    lite3cpp::Buffer local_buf = store->get(key);
    if (local_buf.size() == 0) {
        // No local version, just commit
        Monitor::instance().report_merge_event(false); // Success, no conflict
        return commit_cpb_entry(incoming);
    }


    // Deserialize local version
    CpbEntry local = CpbEntry::deserialize(local_buf);

    if (incoming.better_than(local)) {
        // Incoming WINS. Displace local.
        std::string loser_key = incoming.header.uuid + ":los:" + std::to_string(local.header.timestamp);
        
        // Move local to loser key
        engine_->put_node(loser_key, std::string(reinterpret_cast<const char*>(local_buf.data()), local_buf.size()));
        
        // Register incoming winner
        commit_cpb_entry(incoming);
        
        // Link loser to winner
        engine_->add_edge(loser_key, "SUPERSEDED_BY", 1.0, incoming.header.uuid);
        
        // Telemetry: Merge Conflict (Displacement)
        Monitor::instance().report_merge_event(true);
        return true;
    } else {
        // Local WINS. Store incoming as a loser.
        std::string loser_key = incoming.header.uuid + ":los:" + std::to_string(incoming.header.timestamp);
        
        lite3cpp::Buffer inc_buf;
        incoming.serialize(inc_buf);
        engine_->put_node(loser_key, std::string(reinterpret_cast<const char*>(inc_buf.data()), inc_buf.size()));
        
        // Link incoming loser to local winner
        engine_->add_edge(loser_key, "SUPERSEDED_BY", 1.0, local.header.uuid);
        
        // Telemetry: Merge Conflict (Displacement)
        Monitor::instance().report_merge_event(true);
        return false; // Incoming did not win
    }
}


bool Blackboard::add_wbs_node(const WbsNode& node) {
    lite3cpp::Buffer buf;
    node.serialize(buf);
    
    // Using a separate subspace for WBS nodes
    std::string key = "wbs:" + node.header.id;
    engine_->put_node(key, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    return true;
}

bool Blackboard::apply_delta_patch(const L3DeltaPatch& patch) {
    auto* store = engine_->get_store();
    std::string key(l3kvg::KeyBuilder::node_key(engine_->get_resolver().parse_uuid(patch.header.base_uuid)));
    
    // Retrieve Base
    lite3cpp::Buffer base_buf = store->get(key);
    if (base_buf.size() == 0) {
        std::cerr << "[Blackboard] Cannot apply delta: base node " << patch.header.base_uuid << " not found" << std::endl;
        return false;
    }

    // Apply Patch
    auto reconstructed = DeltaEngine::apply_xor_patch(base_buf, patch);
    if (!reconstructed) return false;

    // Direct put to store (bypass semantic merge as patches are usually during catchup/sync)
    engine_->put_node(patch.header.base_uuid, std::string(reinterpret_cast<const char*>(reconstructed->data()), reconstructed->size()));
    return true;
}

bool Blackboard::commit_identity_node(const IdentityNode& identity) {
    lite3cpp::Buffer buf;
    identity.serialize(buf);
    engine_->put_node(identity.id, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    return true;
}

bool Blackboard::commit_project_node(const ProjectNode& project) {
    lite3cpp::Buffer buf;
    project.serialize(buf);
    engine_->put_node(project.project_id, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    return true;
}

bool Blackboard::commit_engineering_unit(const EngineeringUnit& eu) {

    lite3cpp::Buffer buf;
    eu.serialize(buf);
    
    // EUs are stored in their own space
    std::string key = "eu:" + eu.header.uuid;
    engine_->put_node(key, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    return true;
}

std::vector<std::pair<std::string, std::string>> Blackboard::get_backlinks(const std::string& note_uuid, uint32_t principal_id) {
    if (note_uuid.empty() || !engine_) {
        return {};
    }

    auto key_uuid = engine_->get_resolver().parse_uuid(note_uuid);
    std::string db_key = std::string(l3kvg::KeyBuilder::node_key(key_uuid));
    auto* store = engine_->get_store();

    if (principal_id != 0) {
        auto perm = store->credentials().check_permission(principal_id, db_key);
        if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
            return {};
        }
    }

    char hex_buf[17];
    std::snprintf(hex_buf, sizeof(hex_buf), "%016llx", static_cast<unsigned long long>(key_uuid));
    std::string hex_id(hex_buf);

    std::string prefix = "e:in:{" + hex_id + "}:";
    size_t target_shard = store->get_routing_shard(prefix);
    auto chunk = store->get_prefix_keys(prefix, target_shard, prefix, engine_->get_settings().prefix_scan_limit);

    std::vector<std::pair<std::string, std::string>> backlinks;
    for (const auto& key : chunk) {
        if (key.ends_with(":meta")) continue;

        size_t start_brace = key.find_last_of('{');
        size_t end_brace = key.find_last_of('}');
        if (start_brace == std::string::npos || end_brace == std::string::npos || end_brace <= start_brace) {
            continue;
        }

        if (start_brace <= prefix.size()) continue;
        size_t label_len = (start_brace - 1) - prefix.size();
        std::string label = key.substr(prefix.size(), label_len);
        if (label.empty() || label == rel::CREATED_BY || label == rel::BELONGS_TO) {
            continue;
        }

        std::string src_id_str = key.substr(start_brace + 1, end_brace - start_brace - 1);
        uint64_t src_nid = 0;
        try {
            src_nid = std::stoull(src_id_str, nullptr, 16);
        } catch (...) {
            continue;
        }

        std::string src_uuid = resolve_node_uuid(engine_.get(), src_nid, principal_id);
        auto item = std::make_pair(src_uuid, label);
        if (std::find(backlinks.begin(), backlinks.end(), item) == backlinks.end()) {
            backlinks.push_back(std::move(item));
        }
    }

    return backlinks;
}

std::vector<std::pair<std::string, std::string>> Blackboard::get_outbound_links(const std::string& note_uuid, uint32_t principal_id) {
    if (note_uuid.empty() || !engine_) {
        return {};
    }

    auto key_uuid = engine_->get_resolver().parse_uuid(note_uuid);
    std::string db_key = std::string(l3kvg::KeyBuilder::node_key(key_uuid));
    auto* store = engine_->get_store();

    if (principal_id != 0) {
        auto perm = store->credentials().check_permission(principal_id, db_key);
        if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
            return {};
        }
    }

    char hex_buf[17];
    std::snprintf(hex_buf, sizeof(hex_buf), "%016llx", static_cast<unsigned long long>(key_uuid));
    std::string hex_id(hex_buf);

    std::string prefix = "e:out:{" + hex_id + "}:";
    size_t target_shard = store->get_routing_shard(prefix);
    auto chunk = store->get_prefix_keys(prefix, target_shard, prefix, engine_->get_settings().prefix_scan_limit);

    std::vector<std::pair<std::string, std::string>> outbound;
    for (const auto& key : chunk) {
        if (key.ends_with(":meta")) continue;

        size_t start_brace = key.find_last_of('{');
        size_t end_brace = key.find_last_of('}');
        if (start_brace == std::string::npos || end_brace == std::string::npos || end_brace <= start_brace) {
            continue;
        }

        if (start_brace < 2) continue;
        size_t weight_end = start_brace - 1;
        size_t weight_start = key.find_last_of(':', weight_end - 1);
        if (weight_start == std::string::npos || weight_start <= prefix.size()) {
            continue;
        }

        size_t label_len = weight_start - prefix.size();
        std::string label = key.substr(prefix.size(), label_len);
        if (label.empty() || label == rel::CREATED_BY || label == rel::BELONGS_TO) {
            continue;
        }

        std::string dst_id_str = key.substr(start_brace + 1, end_brace - start_brace - 1);
        uint64_t dst_nid = 0;
        try {
            dst_nid = std::stoull(dst_id_str, nullptr, 16);
        } catch (...) {
            continue;
        }

        std::string dst_uuid = resolve_node_uuid(engine_.get(), dst_nid, principal_id);
        auto item = std::make_pair(dst_uuid, label);
        if (std::find(outbound.begin(), outbound.end(), item) == outbound.end()) {
            outbound.push_back(std::move(item));
        }
    }

    // If any dst_uuid in outbound is an unresolved 16-hex hash, resolve via source note's note_links or references
    uint32_t eff_principal = (principal_id == 0) ? l3kv::ADMIN_UID : principal_id;
    auto src_buf = store->get(db_key, eff_principal);
    if (src_buf.size() > 0) {
        try {
            CpbEntry src_entry = CpbEntry::deserialize(src_buf);
            for (auto& [dst_uuid, rel_label] : outbound) {
                if (dst_uuid.size() == 16) {
                    for (const auto& nl : src_entry.payload.note_links) {
                        if (!nl.target_uuid.empty()) {
                            char hbuf[17];
                            std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                                          static_cast<unsigned long long>(engine_->get_resolver().parse_uuid(nl.target_uuid)));
                            if (dst_uuid == hbuf) {
                                dst_uuid = nl.target_uuid;
                                break;
                            }
                        }
                    }
                    for (const auto& r : src_entry.payload.references) {
                        if (!r.uuid.empty()) {
                            char hbuf[17];
                            std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                                          static_cast<unsigned long long>(engine_->get_resolver().parse_uuid(r.uuid)));
                            if (dst_uuid == hbuf) {
                                dst_uuid = r.uuid;
                                break;
                            }
                        }
                    }
                }
            }
        } catch (...) {}
    }

    return outbound;
}

} // namespace agentic_blackboard

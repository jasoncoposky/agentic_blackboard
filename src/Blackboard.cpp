#include "asos/Blackboard.hpp"
#include "asos/Orchestrator.hpp"
#include "asos/DeltaEngine.hpp"
#include "asos/Monitor.hpp"
#include "buffer.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include "engine/store.hpp"
#include "engine/credential_manager.hpp"
#include <algorithm>

namespace {

const std::vector<std::string>& get_note_link_relation_labels() {
    static const std::vector<std::string> labels = {
        asos::rel::SEE_ALSO,
        asos::rel::REFERENCES,
        asos::rel::CITES,
        asos::rel::SUPPORTS,
        asos::rel::REFUTES,
        asos::rel::EXTENDS,
        asos::rel::SYNTHESIS_OF,
        asos::rel::QUESTION_RAISED_BY,
        asos::rel::ANALOGY_TO,
        asos::rel::PAIRS_WITH,
        asos::rel::VARIATION_OF,
        asos::rel::USES_INGREDIENT,
        asos::rel::DEPENDS_ON,
        asos::rel::BLOCKS,
        asos::rel::SUBTASK_OF,
        asos::rel::VALIDATED_BY,
        asos::rel::CONTRIBUTES_TO
    };
    return labels;
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

std::string resolve_node_uuid(l3kvg::Engine* engine, uint64_t nid) {
    try {
        std::string nkey = std::string(l3kvg::KeyBuilder::node_key(nid));
        auto buf = engine->get_store()->get(nkey);
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

namespace asos {

Blackboard::Blackboard(const std::string& db_path, uint32_t node_id) {
    engine_ = std::make_unique<l3kvg::Engine>(db_path, node_id);
}

Blackboard::~Blackboard() = default;

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
    return hash;
}

bool Blackboard::commit_cpb_entry(const CpbEntry& entry, uint32_t principal_id) {
    CpbEntry adjusted = entry;
    std::cout << "[Blackboard] commit_cpb_entry called for statement: " << adjusted.payload.statement << std::endl;

    // ORPHAN PREVENTION: Every atom must be anchored to both an Agent and a Project
    if (adjusted.header.origin.agent_id.empty() || adjusted.header.origin.project_id.empty()) {
        std::cerr << "[Blackboard] Rejecting Atom " << (adjusted.header.uuid.empty() ? "UNNAMED" : adjusted.header.uuid)
                  << ": Missing mandatory anchors (Agent AND Project required)." << std::endl;
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
    }

    // Security check 1: Ensure user is not impersonating someone else
    if (principal_id != 0 && get_user_uid(author_name) != principal_id) {
        std::cerr << "[Blackboard] Rejecting Atom " << adjusted.header.uuid 
                  << ": Access Denied (Principal ID " << principal_id 
                  << " cannot write on behalf of agent " << author_name << ")" << std::endl;
        return false;
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
        if (!(perm & l3kv::Permission::WRITE) && !(perm & l3kv::Permission::ADMIN)) {
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
    std::cout << "[Blackboard] Committing Atom: " << adjusted.header.uuid << " (Project: " << adjusted.header.origin.project_id << ")" << std::endl;
    engine_->put_node(adjusted.header.uuid, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));

    
    // IDENTITY-CENTRIC ENFORCEMENT: Ensure Links (No Orphans)
    auto author_id = adjusted.header.origin.agent_id;
    auto project_id = adjusted.header.origin.project_id;

    // 1. Link Author (Identity)
    if (!author_id.empty()) {
        auto author_node = engine_->get_node(author_id);
        if (!author_node->has_attribute("header")) {
            IdentityNode stub = {author_id, "Unknown Agent (" + author_id + ")", "STUB", ""};
            commit_identity_node(stub);
        }
        engine_->add_edge(adjusted.header.uuid, rel::CREATED_BY, 1.0, author_id);
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

    // 3. Auto-project Note Links
    for (const auto& link : adjusted.payload.note_links) {
        if (!link.target_uuid.empty()) {
            std::string rel_label = link.relation.empty() ? rel::SEE_ALSO : link.relation;
            engine_->add_edge(adjusted.header.uuid, rel_label, 1.0, link.target_uuid);
            if (principal_id != 0) {
                std::string hex_part = db_key.substr(2);
                creds.set_acl(principal_id, "e:out:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                creds.set_acl(principal_id, "e:in:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);

                auto target_key_uuid = engine_->get_resolver().parse_uuid(link.target_uuid);
                std::string target_db_key = std::string(l3kvg::KeyBuilder::node_key(target_key_uuid));
                std::string target_hex_part = target_db_key.substr(2);
                creds.set_acl(principal_id, "e:out:" + target_hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                creds.set_acl(principal_id, "e:in:" + target_hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
            }
        }
    }

    // 4. Auto-project Reference Citations
    for (const auto& ref : adjusted.payload.references) {
        if (!ref.uuid.empty()) {
            engine_->add_edge(adjusted.header.uuid, rel::CITES, 1.0, ref.uuid);
            if (principal_id != 0) {
                std::string hex_part = db_key.substr(2);
                creds.set_acl(principal_id, "e:out:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                creds.set_acl(principal_id, "e:in:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);

                auto target_key_uuid = engine_->get_resolver().parse_uuid(ref.uuid);
                std::string target_db_key = std::string(l3kvg::KeyBuilder::node_key(target_key_uuid));
                std::string target_hex_part = target_db_key.substr(2);
                creds.set_acl(principal_id, "e:out:" + target_hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                creds.set_acl(principal_id, "e:in:" + target_hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
            }
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

    if (principal_id != 0) {
        auto key_uuid = engine_->get_resolver().parse_uuid(note_uuid);
        std::string db_key = std::string(l3kvg::KeyBuilder::node_key(key_uuid));
        auto perm = engine_->get_store()->credentials().check_permission(principal_id, db_key);
        if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
            return {};
        }
    }

    auto node = engine_->get_node(note_uuid);
    if (!node) {
        return {};
    }

    std::vector<std::pair<std::string, std::string>> backlinks;
    const auto& labels = get_note_link_relation_labels();
    for (const auto& label : labels) {
        auto in_neighbors = node->get_in_neighbors(label, principal_id);
        for (uint64_t src_nid : in_neighbors) {
            std::string src_uuid = resolve_node_uuid(engine_.get(), src_nid);
            auto item = std::make_pair(src_uuid, label);
            if (std::find(backlinks.begin(), backlinks.end(), item) == backlinks.end()) {
                backlinks.push_back(std::move(item));
            }
        }
    }

    return backlinks;
}

std::vector<std::pair<std::string, std::string>> Blackboard::get_outbound_links(const std::string& note_uuid, uint32_t principal_id) {
    if (note_uuid.empty() || !engine_) {
        return {};
    }

    if (principal_id != 0) {
        auto key_uuid = engine_->get_resolver().parse_uuid(note_uuid);
        std::string db_key = std::string(l3kvg::KeyBuilder::node_key(key_uuid));
        auto perm = engine_->get_store()->credentials().check_permission(principal_id, db_key);
        if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
            return {};
        }
    }

    auto node = engine_->get_node(note_uuid);
    if (!node) {
        return {};
    }

    std::vector<std::pair<std::string, std::string>> outbound;
    const auto& labels = get_note_link_relation_labels();
    for (const auto& label : labels) {
        auto edges = node->get_edges(label, -999999.0, principal_id);
        for (const auto& edge : edges) {
            if (!edge) continue;
            uint64_t dst_nid = edge->get_dst();
            std::string dst_uuid = resolve_node_uuid(engine_.get(), dst_nid);
            auto item = std::make_pair(dst_uuid, label);
            if (std::find(outbound.begin(), outbound.end(), item) == outbound.end()) {
                outbound.push_back(std::move(item));
            }
        }
    }

    return outbound;
}

} // namespace asos

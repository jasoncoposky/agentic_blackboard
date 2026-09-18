#include <agentic_blackboard/ApiServer.hpp>
#include <agentic_blackboard/Orchestrator.hpp>
#include <agentic_blackboard/RdfExporter.hpp>
#include "httplib.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <agentic_blackboard/schema.hpp>
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "engine/store.hpp"
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>
#include <random>
#include <chrono>

using json = nlohmann::json;

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

std::string extract_token(const httplib::Request& req) {
    if (req.has_header("Authorization")) {
        std::string auth = req.get_header_value("Authorization");
        if (auth.size() >= 7) {
            std::string prefix = auth.substr(0, 7);
            std::string lower_prefix = prefix;
            std::transform(lower_prefix.begin(), lower_prefix.end(), lower_prefix.begin(), ::tolower);
            if (lower_prefix == "bearer ") {
                std::string tok = auth.substr(7);
                size_t start = tok.find_first_not_of(" \t\r\n");
                size_t end = tok.find_last_not_of(" \t\r\n");
                if (start != std::string::npos && end != std::string::npos) {
                    return tok.substr(start, end - start + 1);
                }
            }
        }
    }
    if (req.has_header("X-AB-Key")) {
        std::string key = req.get_header_value("X-AB-Key");
        size_t start = key.find_first_not_of(" \t\r\n");
        size_t end = key.find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end != std::string::npos) {
            return key.substr(start, end - start + 1);
        }
    }
    if (req.has_param("token")) {
        std::string tok = req.get_param_value("token");
        size_t start = tok.find_first_not_of(" \t\r\n");
        size_t end = tok.find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end != std::string::npos) {
            return tok.substr(start, end - start + 1);
        }
    }
    return "";
}

static inline std::string pure_user_id(const std::string& u) {
    if (u.rfind("user:", 0) == 0) return u.substr(5);
    if (u.rfind("agent:", 0) == 0) return u.substr(6);
    if (u.rfind("identity:", 0) == 0) return u.substr(9);
    return u;
}

static inline std::string canonical_user_id(const std::string& u) {
    if (u.empty()) return "";
    if (u.rfind("user:", 0) == 0) return u;
    return "user:" + u;
}

static inline std::string canonical_surface_id(const std::string& s) {
    if (s.empty()) return "";
    if (s.rfind("surface:", 0) == 0) return s;
    return "surface:" + s;
}

static inline std::string canonical_agent_id(const std::string& a) {
    if (a.empty()) return "";
    if (a.rfind("agent:", 0) == 0) return a;
    return "agent:" + a;
}

} // anonymous namespace

namespace agentic_blackboard {

static std::atomic<httplib::Server*> s_server{nullptr};

bool ContextBroker::register_surface(const std::string& context_id,
                                     const std::string& surface_id,
                                     const std::string& client_app,
                                     const nlohmann::json& capabilities,
                                     nlohmann::json& out_resp) {
    SurfaceInfo info;
    info.surface_id = surface_id;
    info.client_app = client_app;
    info.capabilities = capabilities;
    info.registered_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ContextRecord& ctx = contexts_[context_id];
        ctx.context_id = context_id;
        ctx.surfaces[surface_id] = info;

        auto it = enrolled_surfaces_.find(surface_id);
        if (it != enrolled_surfaces_.end()) {
            it->second.last_seen_sec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            save_enrolled_surfaces_to_store();
        }
    }

    out_resp = {
        {"status", "REGISTERED"},
        {"context_id", context_id},
        {"surface_id", surface_id}
    };

    nlohmann::json evt_payload = {
        {"event", "surface_joined"},
        {"context_id", context_id},
        {"surface_id", surface_id},
        {"client_app", client_app},
        {"capabilities", capabilities}
    };
    broadcast(context_id, "surface_joined", evt_payload.dump());
    return true;
}

bool ContextBroker::get_context_state(const std::string& context_id, nlohmann::json& out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = contexts_.find(context_id);
    if (it == contexts_.end()) {
        return false;
    }
    const auto& ctx = it->second;
    nlohmann::json surfaces_arr = nlohmann::json::array();
    for (const auto& [sid, sinfo] : ctx.surfaces) {
        surfaces_arr.push_back({
            {"surface_id", sinfo.surface_id},
            {"client_app", sinfo.client_app},
            {"capabilities", sinfo.capabilities}
        });
    }
    out_state = {
        {"context_id", ctx.context_id},
        {"active_surfaces", surfaces_arr},
        {"focus", ctx.focus}
    };
    return true;
}

bool ContextBroker::update_focus(const std::string& context_id,
                                 const std::string& surface_id,
                                 const nlohmann::json& focus_payload,
                                 nlohmann::json& out_broadcast_payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ContextRecord& ctx = contexts_[context_id];
        ctx.context_id = context_id;
        ctx.focus = focus_payload;
        if (!surface_id.empty() && !ctx.focus.contains("surface_id")) {
            ctx.focus["surface_id"] = surface_id;
        }
        if (!ctx.focus.contains("selected")) {
            ctx.focus["selected"] = nlohmann::json::array();
        }
        if (!ctx.focus.contains("context_id")) {
            ctx.focus["context_id"] = context_id;
        }
        if (!surface_id.empty()) {
            auto it = enrolled_surfaces_.find(surface_id);
            if (it == enrolled_surfaces_.end()) {
                it = enrolled_surfaces_.find(canonical_surface_id(surface_id));
            }
            if (it != enrolled_surfaces_.end()) {
                it->second.last_seen_sec = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            }
        }
        out_broadcast_payload = ctx.focus;
    }

    broadcast(context_id, "focus_update", out_broadcast_payload.dump());
    return true;
}

std::shared_ptr<SseClientSession> ContextBroker::create_client(const std::string& context_id) {
    auto session = std::make_shared<SseClientSession>();
    session->id = next_client_id_++;
    session->context_id = context_id;
    session->active = true;

    nlohmann::json handshake = {
        {"status", "connected"},
        {"context_id", context_id}
    };
    std::string init_evt = "retry: 3000\n\nevent: connected\ndata: " + handshake.dump() + "\n\n";
    session->event_queue.push(init_evt);

    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_[context_id][session->id] = session;
    return session;
}

void ContextBroker::remove_client(const std::string& context_id, uint64_t client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(context_id);
    if (it != subscribers_.end()) {
        it->second.erase(client_id);
        if (it->second.empty()) {
            subscribers_.erase(it);
        }
    }
}

void ContextBroker::broadcast(const std::string& context_id, const std::string& event_name, const std::string& json_data) {
    std::string sse_msg = "event: " + event_name + "\ndata: " + json_data + "\n\n";
    std::vector<std::shared_ptr<SseClientSession>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(context_id);
        if (it != subscribers_.end()) {
            targets.reserve(it->second.size());
            for (auto& [cid, session] : it->second) {
                targets.push_back(session);
            }
        }
    }

    for (auto& session : targets) {
        if (!session->active) continue;
        {
            std::lock_guard<std::mutex> slock(session->mutex);
            if (!session->active) continue;
            if (session->event_queue.size() >= SseClientSession::kMaxQueueSize) {
                session->event_queue.pop();
            }
            session->event_queue.push(sse_msg);
        }
        session->cv.notify_one();
    }
}

void ContextBroker::shutdown() {
    std::vector<std::shared_ptr<SseClientSession>> all_sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [ctx_id, clients] : subscribers_) {
            for (auto& [cid, session] : clients) {
                all_sessions.push_back(session);
            }
        }
        subscribers_.clear();
        contexts_.clear();
    }
    for (auto& session : all_sessions) {
        {
            std::lock_guard<std::mutex> slock(session->mutex);
            session->active = false;
        }
        session->cv.notify_all();
    }
}

void ContextBroker::set_blackboard(Blackboard* bb) {
    blackboard_ = bb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enrolled_surfaces_.clear();
    }
    if (blackboard_) {
        try {
            auto* store = blackboard_->get_engine()->get_store();
            if (store) {
                lite3cpp::Buffer buf = store->get("auth:registry:surfaces", l3kv::ADMIN_UID);
                if (buf.size() > 0) {
                    std::string js;
                    try {
                        js = lite3cpp::lite3_json::to_json_string(buf, 0);
                    } catch (...) {}
                    if (js.empty() || js == "[]" || js == "{}" || js == "null") {
                        js = std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
                    }
                    size_t start = js.find_first_not_of(" \t\r\n");
                    if (start != std::string::npos) {
                        js = js.substr(start);
                    }
                    if (!js.empty() && (js[0] == '[' || js[0] == '{')) {
                        auto parsed = nlohmann::json::parse(js);
                        auto arr = parsed.is_array() ? parsed : (parsed.contains("surfaces") && parsed["surfaces"].is_array() ? parsed["surfaces"] : nlohmann::json::array());
                        if (arr.is_array()) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            enrolled_surfaces_.clear();
                            for (const auto& item : arr) {
                                EnrolledSurface s;
                                s.surface_id = item.value("surface_id", "");
                                s.surface_type = item.value("surface_type", "");
                                s.client_app = item.value("client_app", "");
                                s.user_id = item.value("user_id", "");
                                s.agent_id = item.value("agent_id", "");
                                s.context_id = item.value("context_id", "");
                                s.token_hash = item.value("token_hash", "");
                                s.enrolled_at_sec = item.value("enrolled_at", 0LL);
                                s.last_seen_sec = item.value("last_seen", 0LL);
                                if (s.last_seen_sec == 0) {
                                    s.last_seen_sec = s.enrolled_at_sec;
                                }
                                if (!s.surface_id.empty()) {
                                    enrolled_surfaces_[s.surface_id] = s;
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {}
    }
}

void ContextBroker::save_enrolled_surfaces_to_store() {
    if (!blackboard_) return;
    auto* store = blackboard_->get_engine()->get_store();
    if (!store) return;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [sid, s] : enrolled_surfaces_) {
        arr.push_back({
            {"surface_id", s.surface_id},
            {"surface_type", s.surface_type},
            {"client_app", s.client_app},
            {"user_id", s.user_id},
            {"agent_id", s.agent_id},
            {"context_id", s.context_id},
            {"token_hash", s.token_hash},
            {"enrolled_at", s.enrolled_at_sec},
            {"last_seen", s.last_seen_sec}
        });
    }
    store->put("auth:registry:surfaces", " " + arr.dump());
    store->wait_all_shards();
}

bool ContextBroker::request_surface_pairing(const std::string& surface_type,
                                            const std::string& client_app,
                                            const std::string& suggested_id,
                                            nlohmann::json& out_resp) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<int> pin_dist(0, 999999);
    char pin_buf[16];
    std::snprintf(pin_buf, sizeof(pin_buf), "%06d", pin_dist(gen));
    std::string pin = pin_buf;

    std::uniform_int_distribution<uint64_t> hex_dist;
    uint64_t r1 = hex_dist(gen);
    uint64_t r2 = hex_dist(gen);
    std::ostringstream pid_oss;
    pid_oss << "pair_" << std::hex << std::setw(16) << std::setfill('0') << r1
            << std::setw(16) << std::setfill('0') << r2;
    std::string pairing_id = pid_oss.str();

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t expires_at = now + 300;

    PairingSession session;
    session.pairing_id = pairing_id;
    session.pin = pin;
    session.surface_type = surface_type.empty() ? "unknown" : surface_type;
    session.client_app = client_app;
    session.suggested_id = suggested_id;
    session.created_at_sec = now;
    session.expires_at_sec = expires_at;
    session.failed_attempts = 0;
    session.approved = false;
    session.claimed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pairing_sessions_.begin(); it != pairing_sessions_.end(); ) {
            if (now > it->second.expires_at_sec || it->second.claimed) {
                it = pairing_sessions_.erase(it);
            } else {
                ++it;
            }
        }
        pairing_sessions_[pairing_id] = session;
    }

    out_resp = {
        {"status", "PENDING"},
        {"pairing_id", pairing_id},
        {"pin", pin},
        {"expires_at", expires_at},
        {"expires_in_sec", 300},
        {"surface_type", session.surface_type},
        {"suggested_id", suggested_id}
    };
    return true;
}

bool ContextBroker::approve_surface_pairing(const std::string& pairing_id,
                                            const std::string& pin,
                                            const std::string& user_id,
                                            const std::string& agent_id,
                                            const std::string& context_id,
                                            const std::string& surface_id,
                                            nlohmann::json& out_resp) {
    std::string actual_surface_id;
    std::string surface_token;
    std::string token_hash;
    std::string surface_type;
    std::string client_app;
    int64_t now = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pairing_sessions_.find(pairing_id);
        if (it == pairing_sessions_.end()) {
            out_resp = {{"error", "Pairing session not found"}};
            return false;
        }

        auto& session = it->second;
        if (session.claimed) {
            out_resp = {{"error", "Pairing session already claimed or expired"}};
            return false;
        }

        if (session.approved) {
            out_resp = {{"error", "Pairing session already approved"}};
            return false;
        }

        now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (now > session.expires_at_sec) {
            pairing_sessions_.erase(it);
            out_resp = {{"error", "Pairing session expired"}};
            return false;
        }

        if (session.pin != pin) {
            session.failed_attempts++;
            if (session.failed_attempts >= 3) {
                pairing_sessions_.erase(it);
                out_resp = {{"error", "Maximum PIN verification attempts exceeded; session invalidated"}};
                return false;
            }
            out_resp = {{"error", "Invalid PIN"}, {"remaining_attempts", 3 - session.failed_attempts}};
            return false;
        }

        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> hex_dist;
        uint64_t r1 = hex_dist(gen);
        uint64_t r2 = hex_dist(gen);
        std::ostringstream tok_oss;
        tok_oss << "ab_srf_"
                << std::hex << std::setw(16) << std::setfill('0') << r1
                << std::setw(16) << std::setfill('0') << r2;
        surface_token = tok_oss.str();
        token_hash = hash_token_sha256(surface_token);

        if (!surface_id.empty()) {
            actual_surface_id = canonical_surface_id(surface_id);
        } else if (!session.suggested_id.empty()) {
            actual_surface_id = canonical_surface_id(session.suggested_id);
        } else {
            actual_surface_id = "surface:" + session.pairing_id;
        }

        auto existing_it = enrolled_surfaces_.find(actual_surface_id);
        if (existing_it != enrolled_surfaces_.end()) {
            std::string old_token_hash = existing_it->second.token_hash;
            if (!old_token_hash.empty() && blackboard_) {
                auto* store = blackboard_->get_engine()->get_store();
                if (store) {
                    store->del("auth:token:" + old_token_hash);
                    store->wait_all_shards();
                }
            }
        }

        surface_type = session.surface_type;
        client_app = session.client_app;
        std::string canon_user = canonical_user_id(user_id);

        session.approved = true;
        session.approved_user_id = canon_user;
        session.approved_agent_id = agent_id;
        session.assigned_surface_id = actual_surface_id;
        session.assigned_context_id = context_id;
        session.surface_token = surface_token;

        EnrolledSurface enrolled;
        enrolled.surface_id = actual_surface_id;
        enrolled.surface_type = surface_type;
        enrolled.client_app = client_app;
        enrolled.user_id = canon_user;
        enrolled.agent_id = agent_id;
        enrolled.context_id = context_id;
        enrolled.token_hash = token_hash;
        enrolled.enrolled_at_sec = now;
        enrolled.last_seen_sec = now;
        enrolled_surfaces_[actual_surface_id] = enrolled;
        save_enrolled_surfaces_to_store();
    }

    if (blackboard_) {
        std::string canon_user = canonical_user_id(user_id);
        blackboard_->register_user_credentials(canon_user, canon_user + "-key");
        if (pure_user_id(canon_user) != canon_user) {
            blackboard_->register_user_credentials(pure_user_id(canon_user), pure_user_id(canon_user) + "-key");
        }
        auto* store = blackboard_->get_engine()->get_store();
        if (store) {
            std::string db_key = "auth:token:" + token_hash;
            nlohmann::json meta = {
                {"username", canon_user},
                {"role", "surface"},
                {"agent", agent_id},
                {"surface_id", actual_surface_id},
                {"surface_type", surface_type},
                {"context_id", context_id},
                {"created_at", now}
            };
            store->put(db_key, meta.dump());
            store->wait_all_shards();
        }
    }

    out_resp = {
        {"status", "APPROVED"},
        {"pairing_id", pairing_id},
        {"surface_id", actual_surface_id},
        {"user_id", canonical_user_id(user_id)},
        {"agent_id", agent_id},
        {"context_id", context_id}
    };
    return true;
}

bool ContextBroker::claim_surface_pairing(const std::string& pairing_id,
                                          nlohmann::json& out_resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pairing_sessions_.find(pairing_id);
    if (it == pairing_sessions_.end()) {
        out_resp = {{"error", "Pairing session not found"}};
        return false;
    }

    auto& session = it->second;
    if (session.claimed) {
        out_resp = {{"error", "Pairing session already claimed or expired"}};
        return false;
    }

    if (!session.approved) {
        out_resp = {{"error", "Pairing session not yet approved"}};
        return false;
    }

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now > session.expires_at_sec) {
        pairing_sessions_.erase(it);
        out_resp = {{"error", "Pairing session expired"}};
        return false;
    }

    session.claimed = true;
    out_resp = {
        {"status", "CLAIMED"},
        {"surface_token", session.surface_token},
        {"surface_id", session.assigned_surface_id},
        {"surface_type", session.surface_type},
        {"user_id", session.approved_user_id},
        {"agent_id", session.approved_agent_id},
        {"context_id", session.assigned_context_id}
    };
    pairing_sessions_.erase(it);
    return true;
}

bool ContextBroker::list_enrolled_surfaces(const std::string& user_id, nlohmann::json& out_resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [sid, s] : enrolled_surfaces_) {
        if (user_id.empty() || pure_user_id(user_id) == "admin" || pure_user_id(s.user_id) == pure_user_id(user_id)) {
            arr.push_back({
                {"surface_id", s.surface_id},
                {"surface_type", s.surface_type},
                {"client_app", s.client_app},
                {"user_id", s.user_id},
                {"agent_id", s.agent_id},
                {"context_id", s.context_id},
                {"enrolled_at", s.enrolled_at_sec},
                {"last_seen", s.last_seen_sec}
            });
        }
    }
    out_resp = {
        {"status", "OK"},
        {"surfaces", arr}
    };
    return true;
}

bool ContextBroker::revoke_enrolled_surface(const std::string& surface_id, nlohmann::json& out_resp) {
    std::string token_hash;
    std::string actual_id = surface_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = enrolled_surfaces_.find(surface_id);
        if (it == enrolled_surfaces_.end()) {
            it = enrolled_surfaces_.find(canonical_surface_id(surface_id));
        }
        if (it == enrolled_surfaces_.end()) {
            out_resp = {{"error", "Surface not found"}, {"surface_id", surface_id}};
            return false;
        }

        actual_id = it->first;
        token_hash = it->second.token_hash;
        enrolled_surfaces_.erase(it);
        save_enrolled_surfaces_to_store();
    }

    if (!token_hash.empty() && blackboard_) {
        auto* store = blackboard_->get_engine()->get_store();
        if (store) {
            std::string db_key = "auth:token:" + token_hash;
            store->del(db_key);
            store->wait_all_shards();
        }
    }

    out_resp = {
        {"status", "REVOKED"},
        {"surface_id", actual_id}
    };
    return true;
}

bool ContextBroker::get_enrolled_surface(const std::string& surface_id, EnrolledSurface& out_surface) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = enrolled_surfaces_.find(surface_id);
    if (it == enrolled_surfaces_.end()) {
        it = enrolled_surfaces_.find(canonical_surface_id(surface_id));
    }
    if (it == enrolled_surfaces_.end()) {
        return false;
    }
    out_surface = it->second;
    return true;
}

void ContextBroker::touch_surface(const std::string& surface_id) {
    if (surface_id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = enrolled_surfaces_.find(surface_id);
    if (it == enrolled_surfaces_.end()) {
        it = enrolled_surfaces_.find(canonical_surface_id(surface_id));
    }
    if (it != enrolled_surfaces_.end()) {
        it->second.last_seen_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

ApiServer::~ApiServer() {
    stop();
}

void ApiServer::start(Blackboard* blackboard, int port, const std::string& host) {
    blackboard_ = blackboard;
    context_broker_.set_blackboard(blackboard);
    port_ = port;
    host_ = host.empty() ? "0.0.0.0" : host;
    running_ = true;
    thread_ = std::thread(&ApiServer::listen_loop, this);
}

void ApiServer::stop() {
    if (!running_) return;
    running_ = false;
    context_broker_.shutdown();
    auto* s = s_server.load();
    if (s) {
        s->stop();
    }
    if (thread_.joinable())
        thread_.join();
    s_server.store(nullptr);
}

bool ApiServer::authenticate_request(const httplib::Request& req, httplib::Response& res,
                                     uint32_t& principal_id, std::string& authenticated_user,
                                     std::string& authenticated_role,
                                     nlohmann::json* out_token_meta) {
    if (!blackboard_) {
        res.status = 503;
        json err = {
            {"error", "Service Unavailable"},
            {"message", "Blackboard instance not initialized"}
        };
        res.set_content(err.dump(), "application/json");
        return false;
    }

    std::string auth_mode = blackboard_->get_auth_mode();
    nlohmann::json token_meta;
    if (auth_mode == "token") {
        std::string token = extract_token(req);
        if (token.empty() || !blackboard_->validate_token(token, authenticated_user, authenticated_role, token_meta)) {
            res.status = 401;
            json err = {
                {"error", "Unauthorized"},
                {"message", "Valid Bearer token required"}
            };
            res.set_content(err.dump(), "application/json");
            return false;
        }
        if (out_token_meta) {
            *out_token_meta = token_meta;
        }
        std::string pure_u = pure_user_id(authenticated_user);
        std::string canon_u = canonical_user_id(authenticated_user);
        blackboard_->register_user_credentials(authenticated_user, authenticated_user + "-key");
        blackboard_->register_user_credentials(pure_u, pure_u + "-key");
        blackboard_->register_user_credentials(canon_u, canon_u + "-key");

        if (authenticated_user == "admin" || authenticated_role == "admin") {
            principal_id = 0;
        } else if (authenticated_role == "surface") {
            principal_id = blackboard_->get_user_uid(pure_u);
            std::string sid = token_meta.value("surface_id", "");
            if (!sid.empty()) {
                context_broker_.touch_surface(sid);
            }
        } else {
            principal_id = blackboard_->get_user_uid(pure_u);
        }
        return true;
    } else {
        // trusted_network mode (fallback)
        std::string token = extract_token(req);
        if (!token.empty() && blackboard_->validate_token(token, authenticated_user, authenticated_role, token_meta)) {
            if (out_token_meta) {
                *out_token_meta = token_meta;
            }
            std::string pure_u = pure_user_id(authenticated_user);
            std::string canon_u = canonical_user_id(authenticated_user);
            blackboard_->register_user_credentials(authenticated_user, authenticated_user + "-key");
            blackboard_->register_user_credentials(pure_u, pure_u + "-key");
            blackboard_->register_user_credentials(canon_u, canon_u + "-key");

            if (authenticated_user == "admin" || authenticated_role == "admin") {
                principal_id = 0;
            } else if (authenticated_role == "surface") {
                principal_id = blackboard_->get_user_uid(pure_u);
                std::string sid = token_meta.value("surface_id", "");
                if (!sid.empty()) {
                    context_broker_.touch_surface(sid);
                }
            } else {
                principal_id = blackboard_->get_user_uid(pure_u);
            }
            return true;
        }

        std::string active_user = req.get_header_value("X-Active-User");
        authenticated_user = active_user;
        authenticated_role = (active_user == "admin") ? "admin" : "user";
        principal_id = 0;
        if (!active_user.empty() && active_user != "admin") {
            std::string pure_u = pure_user_id(active_user);
            std::string canon_u = canonical_user_id(active_user);
            blackboard_->register_user_credentials(active_user, active_user + "-key");
            blackboard_->register_user_credentials(pure_u, pure_u + "-key");
            blackboard_->register_user_credentials(canon_u, canon_u + "-key");
            principal_id = blackboard_->get_user_uid(pure_u);
        }
        return true;
    }
}

void ApiServer::listen_loop() {
    httplib::Server svr;
    s_server.store(&svr);

    // Enable CORS for Dashboard
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-Active-User, X-AB-Key"}
    });

    svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // 1. Schema Discovery Endpoint
    svr.Get("/api/v1/schema", [this](const httplib::Request&, httplib::Response& res) {
        std::cout << "[API] GET /api/v1/schema" << std::endl;
        json schema = {
            {"system", "Agentic Blackboard v0.4-α"},
            {"knowledge_areas", {
                {"0", "UNKNOWN"},
                {"1", "REQUIREMENTS"},
                {"2", "DESIGN"},
                {"3", "CONSTRUCTION"},
                {"4", "TESTING"},
                {"5", "MAINTENANCE"},
                {"6", "CONFIG_MANAGEMENT"},
                {"7", "ENGINEERING_MANAGEMENT"},
                {"8", "ENGINEERING_PROCESS"},
                {"9", "ENGINEERING_MODELS"},
                {"10", "QUALITY"},
                {"11", "PROFESSIONAL_PRACTICE"},
                {"12", "ECONOMICS"},
                {"13", "COMPUTING_FOUNDATIONS"},
                {"14", "MATHEMATICAL_FOUNDATIONS"},
                {"15", "ENGINEERING_FOUNDATIONS"},
                {"20", "HEALTH_WELLNESS"},
                {"21", "SOCIAL_RELATIONSHIPS"},
                {"22", "PERSONAL_REFLECTIONS"},
                {"23", "LEISURE_CREATIVITY"},
                {"24", "DAILY_ROUTINE"},
                {"25", "EDUCATION_LEARNING"},
                {"26", "LITERATURE_READING"},
                {"27", "CULINARY_RECIPES"},
                {"28", "CREATIVE_ARTS"},
                {"29", "PERSONAL_FINANCE"},
                {"30", "HOME_LOGISTICS"},
                {"31", "GENERAL_COMMONPLACE"}
            }},
            {"relationships", {
                rel::CREATED_BY, rel::BELONGS_TO, rel::MAINTAINS, rel::SUPERSEDED_BY, rel::CPB_SIMILARITY, rel::RELATED_TO,
                rel::REPRESENTED_BY, rel::ANCHORED_TO, rel::MENTIONS, rel::OCCURRED_AT,
                rel::DEPENDS_ON, rel::BLOCKS, rel::SUBTASK_OF, rel::VALIDATED_BY, rel::CONTRIBUTES_TO,
                rel::SEE_ALSO, rel::REFERENCES, rel::CITES, rel::SUPPORTS, rel::REFUTES, rel::EXTENDS, rel::SYNTHESIS_OF,
                rel::QUESTION_RAISED_BY, rel::ANALOGY_TO,
                rel::PAIRS_WITH, rel::VARIATION_OF, rel::USES_INGREDIENT
            }},
            {"types", {
                {"CPB_ENTRY", {{"fields", {"uuid", "statement", "content", "ka", "tags", "applicability", "references", "note_links", "items", "steps", "metrics", "attributes"}}}},
                {"REFERENCE", {{"fields", {"title", "page_numbers", "uuid", "creator", "tags", "excerpt"}}}},
                {"NOTE_LINK", {{"fields", {"target_uuid", "relation", "context"}}}},
                {"CATALOG_ITEM", {{"fields", {"name", "quantity", "unit", "role", "notes"}}}},
                {"CATALOG_STEP", {{"fields", {"step_number", "instruction", "duration_minutes", "required_tools", "prerequisites"}}}},
                {"CATALOG_METRIC", {{"fields", {"name", "value", "unit"}}}},
                {"IDENTITY", {{"fields", {"id", "display_name", "role", "public_key", "content"}}}},
                {"PROJECT", {{"fields", {"project_id", "description", "lifecycle_status", "content"}}}}
            }}
        };

        res.set_content(schema.dump(2), "application/json");
    });

    // 2. Atomic Graph Bundle Commit (Orphan Prevention)
    // Expects JSON: { "atoms": [...], "project_id": "...", "agent_id": "..." }
    svr.Post("/api/v1/graph/bundle", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            nlohmann::json token_meta;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role, &token_meta)) {
                return;
            }

            auto j = json::parse(req.body);
            json atoms_array;
            if (j.contains("atoms") && j["atoms"].is_array()) {
                atoms_array = j["atoms"];
            } else if (j.contains("atom") && j["atom"].is_object()) {
                atoms_array = json::array({ j["atom"] });
            } else {
                res.status = 400;
                res.set_content("Error: Missing atoms array", "text/plain");
                return;
            }

            std::string top_project_id = j.value("project_id", "");
            std::string top_agent_id = j.value("agent_id", "");

            std::string token_user;
            std::string token_agent;
            std::string token_surface_id;
            std::string token_surface_type;
            std::string token_context_id;

            if (auth_role == "surface") {
                token_user = token_meta.value("username", "");
                if (token_user.empty()) token_user = active_user;
                token_agent = token_meta.value("agent", "");
                if (token_agent.empty()) token_agent = token_meta.value("agent_id", "");
                if (token_agent.empty() && !token_user.empty()) token_agent = "agent:" + pure_user_id(token_user) + "-agent";
                token_surface_id = token_meta.value("surface_id", "");
                token_surface_type = token_meta.value("surface_type", "");
                token_context_id = token_meta.value("context_id", "");
            }

            auto get_origin_field = [](const json& item, const std::string& key) -> std::string {
                if (item.contains("header") && item["header"].is_object() &&
                    item["header"].contains("origin") && item["header"]["origin"].is_object() &&
                    item["header"]["origin"].contains(key)) {
                    return item["header"]["origin"].value(key, "");
                }
                if (item.contains("origin") && item["origin"].is_object() &&
                    item["origin"].contains(key)) {
                    return item["origin"].value(key, "");
                }
                if (item.contains(key) && item[key].is_string()) {
                    return item.value(key, "");
                }
                return "";
            };

            int count = 0;
            for (auto& item : atoms_array) {
                CpbEntry e;
                // Support both flat and nested header structures
                if (item.contains("header") && item["header"].is_object() && item["header"].contains("uuid")) {
                    e.header.uuid = item["header"].value("uuid", "");
                } else if (item.contains("uuid")) {
                    e.header.uuid = item.value("uuid", "");
                } else if (item.contains("id")) {
                    e.header.uuid = item.value("id", "");
                }

                std::string orig_user_id = get_origin_field(item, "user_id");
                std::string orig_agent_id = get_origin_field(item, "agent_id");
                std::string orig_surface_id = get_origin_field(item, "surface_id");
                std::string orig_surface_type = get_origin_field(item, "surface_type");
                std::string orig_context_id = get_origin_field(item, "context_id");
                if (orig_context_id.empty() && j.contains("context_id")) {
                    orig_context_id = j.value("context_id", "");
                }
                std::string orig_project_id = get_origin_field(item, "project_id");

                if (auth_role == "surface") {
                    if (!orig_user_id.empty() && pure_user_id(orig_user_id) != pure_user_id(token_user)) {
                        res.status = 403;
                        res.set_content(json({
                            {"error", "Forbidden"},
                            {"message", "Surface token cannot commit for user: " + orig_user_id}
                        }).dump(), "application/json");
                        return;
                    }
                    if (!orig_surface_id.empty() && canonical_surface_id(orig_surface_id) != canonical_surface_id(token_surface_id)) {
                        res.status = 403;
                        res.set_content(json({
                            {"error", "Forbidden"},
                            {"message", "Surface token cannot commit for surface: " + orig_surface_id}
                        }).dump(), "application/json");
                        return;
                    }
                    if (!orig_agent_id.empty() && !token_agent.empty() && pure_user_id(orig_agent_id) != pure_user_id(token_agent)) {
                        res.status = 403;
                        res.set_content(json({
                            {"error", "Forbidden"},
                            {"message", "Surface token cannot commit for agent: " + orig_agent_id}
                        }).dump(), "application/json");
                        return;
                    }

                    e.header.origin.user_id = canonical_user_id(token_user);
                    e.header.origin.surface_id = canonical_surface_id(token_surface_id);
                    e.header.origin.agent_id = token_agent;
                    e.header.origin.surface_type = orig_surface_type.empty() ? token_surface_type : orig_surface_type;
                    e.header.origin.context_id = orig_context_id.empty() ? token_context_id : orig_context_id;
                    if (!orig_project_id.empty()) {
                        e.header.origin.project_id = orig_project_id;
                    } else if (!top_project_id.empty()) {
                        e.header.origin.project_id = top_project_id;
                    } else {
                        e.header.origin.project_id = "proj-default";
                    }
                } else if (auth_role == "user") {
                    if (!orig_user_id.empty() && pure_user_id(orig_user_id) != pure_user_id(active_user)) {
                        res.status = 403;
                        res.set_content(json({
                            {"error", "Forbidden"},
                            {"message", "User cannot commit for user: " + orig_user_id}
                        }).dump(), "application/json");
                        return;
                    }

                    e.header.origin.user_id = canonical_user_id(active_user);
                    if (!orig_agent_id.empty()) {
                        e.header.origin.agent_id = orig_agent_id;
                    } else if (!top_agent_id.empty()) {
                        e.header.origin.agent_id = top_agent_id;
                    } else {
                        e.header.origin.agent_id = "agent:" + pure_user_id(active_user) + "-agent";
                    }

                    e.header.origin.surface_id = orig_surface_id;
                    e.header.origin.surface_type = orig_surface_type;
                    e.header.origin.context_id = orig_context_id;
                    if (!orig_project_id.empty()) {
                        e.header.origin.project_id = orig_project_id;
                    } else if (!top_project_id.empty()) {
                        e.header.origin.project_id = top_project_id;
                    } else {
                        e.header.origin.project_id = "proj-default";
                    }
                } else {
                    // admin
                    e.header.origin.user_id = orig_user_id.empty() ? canonical_user_id(active_user) : orig_user_id;
                    if (!orig_agent_id.empty()) {
                        e.header.origin.agent_id = orig_agent_id;
                    } else if (!top_agent_id.empty()) {
                        e.header.origin.agent_id = top_agent_id;
                    } else {
                        e.header.origin.agent_id = "agent:admin-agent";
                    }

                    e.header.origin.surface_id = orig_surface_id;
                    e.header.origin.surface_type = orig_surface_type;
                    e.header.origin.context_id = orig_context_id;
                    if (!orig_project_id.empty()) {
                        e.header.origin.project_id = orig_project_id;
                    } else if (!top_project_id.empty()) {
                        e.header.origin.project_id = top_project_id;
                    } else {
                        e.header.origin.project_id = "proj-default";
                    }
                }

                if (e.header.origin.project_id.empty() || (e.header.origin.user_id.empty() && e.header.origin.agent_id.empty())) {
                    res.status = 400;
                    res.set_content("Error: Missing mandatory Project or Identity anchor.", "text/plain");
                    return;
                }

                if (item.contains("payload")) {
                    e.payload.statement = item["payload"].value("statement", "");
                    e.payload.content = item["payload"].value("content", "");
                } else {
                    e.payload.statement = item.value("statement", "");
                    e.payload.content = item.value("content", "");
                }

                if (item.contains("taxonomy")) {
                    e.taxonomy.knowledge_area = static_cast<KnowledgeArea>(item["taxonomy"].value("ka", 0));
                    e.taxonomy.applicability = item["taxonomy"].value("applicability", 50);
                    if (item["taxonomy"].contains("tags")) {
                        for (auto& tag : item["taxonomy"]["tags"]) e.taxonomy.tags.push_back(tag.get<std::string>());
                    }
                } else {
                    e.taxonomy.knowledge_area = static_cast<KnowledgeArea>(item.value("ka", 0));
                    e.taxonomy.applicability = item.value("applicability", 50);
                    if (item.contains("tags")) {
                        for (auto& tag : item["tags"]) e.taxonomy.tags.push_back(tag.get<std::string>());
                    }
                }

                if (item.contains("wellness")) {
                    Wellness w;
                    w.mood_sentiment = item["wellness"].value("mood_sentiment", 0.0);
                    w.energy_level = item["wellness"].value("energy_level", 0.0);
                    w.sleep_hours = item["wellness"].value("sleep_hours", 0.0);
                    w.active_minutes = item["wellness"].value("active_minutes", 0.0);
                    w.step_count = item["wellness"].value("step_count", static_cast<int64_t>(0));
                    w.activity_type = item["wellness"].value("activity_type", "");
                    e.wellness = w;
                }

                if (item.contains("education")) {
                    Education edu;
                    edu.institution_platform = item["education"].value("institution_platform", "");
                    edu.resource_type = item["education"].value("resource_type", "");
                    edu.progress_percent = item["education"].value("progress_percent", 0.0);
                    edu.focus_duration_minutes = item["education"].value("focus_duration_minutes", 0.0);
                    edu.credential_uuid = item["education"].value("credential_uuid", "");
                    e.education = edu;
                }

                // References
                auto parse_references = [&](const json& refs_json) {
                    for (const auto& r_json : refs_json) {
                        Reference r;
                        r.title = r_json.value("title", "");
                        r.page_numbers = r_json.value("page_numbers", "");
                        r.uuid = r_json.value("uuid", "");
                        r.creator = r_json.value("creator", "");
                        if (r_json.contains("tags") && r_json["tags"].is_array()) {
                            for (auto& tag : r_json["tags"]) r.tags.push_back(tag.get<std::string>());
                        }
                        r.excerpt = r_json.value("excerpt", "");
                        e.payload.references.push_back(r);
                    }
                };
                if (item.contains("payload") && item["payload"].contains("references") && item["payload"]["references"].is_array()) {
                    parse_references(item["payload"]["references"]);
                } else if (item.contains("references") && item["references"].is_array()) {
                    parse_references(item["references"]);
                }

                // Note Links
                auto parse_note_links = [&](const json& links_json) {
                    for (const auto& l_json : links_json) {
                        NoteLink nl;
                        nl.target_uuid = l_json.value("target_uuid", "");
                        nl.relation = l_json.value("relation", "");
                        nl.context = l_json.value("context", "");
                        e.payload.note_links.push_back(nl);
                    }
                };
                if (item.contains("payload") && item["payload"].contains("note_links") && item["payload"]["note_links"].is_array()) {
                    parse_note_links(item["payload"]["note_links"]);
                } else if (item.contains("note_links") && item["note_links"].is_array()) {
                    parse_note_links(item["note_links"]);
                }

                // Items
                if (item.contains("items") && item["items"].is_array()) {
                    for (const auto& it_json : item["items"]) {
                        CatalogItem ci;
                        ci.name = it_json.value("name", "");
                        ci.quantity = it_json.value("quantity", 0.0);
                        ci.unit = it_json.value("unit", "");
                        ci.role = it_json.value("role", "");
                        ci.notes = it_json.value("notes", "");
                        e.items.push_back(ci);
                    }
                }

                // Steps
                if (item.contains("steps") && item["steps"].is_array()) {
                    for (const auto& st_json : item["steps"]) {
                        CatalogStep cs;
                        cs.step_number = st_json.value("step_number", 1);
                        cs.instruction = st_json.value("instruction", "");
                        if (st_json.contains("duration_seconds")) {
                            cs.duration_seconds = st_json.value("duration_seconds", 0);
                        } else if (st_json.contains("duration_minutes")) {
                            cs.duration_seconds = static_cast<int32_t>(st_json.value("duration_minutes", 0.0) * 60);
                        }
                        cs.notes = st_json.value("notes", "");
                        if (st_json.contains("required_tools") && st_json["required_tools"].is_array()) {
                            std::string tools_str;
                            for (auto& t : st_json["required_tools"]) {
                                if (!tools_str.empty()) tools_str += ", ";
                                tools_str += t.get<std::string>();
                            }
                            if (!tools_str.empty()) {
                                if (!cs.notes.empty()) cs.notes += " | ";
                                cs.notes += "Tools: " + tools_str;
                            }
                        }
                        if (st_json.contains("prerequisites") && st_json["prerequisites"].is_array()) {
                            std::string prereq_str;
                            for (auto& p : st_json["prerequisites"]) {
                                if (!prereq_str.empty()) prereq_str += ", ";
                                prereq_str += p.get<std::string>();
                            }
                            if (!prereq_str.empty()) {
                                if (!cs.notes.empty()) cs.notes += " | ";
                                cs.notes += "Prereqs: " + prereq_str;
                            }
                        }
                        e.steps.push_back(cs);
                    }
                }

                // Metrics
                if (item.contains("metrics") && item["metrics"].is_array()) {
                    for (const auto& m_json : item["metrics"]) {
                        CatalogMetric cm;
                        cm.key = m_json.value("name", "");
                        if (cm.key.empty()) cm.key = m_json.value("key", "");
                        cm.value = m_json.value("value", 0.0);
                        cm.unit = m_json.value("unit", "");
                        e.metrics.push_back(cm);
                    }
                }

                // Attributes
                if (item.contains("attributes") && item["attributes"].is_object()) {
                    for (auto& [k, v] : item["attributes"].items()) {
                        if (v.is_string()) {
                            e.attributes[k] = v.get<std::string>();
                        } else {
                            e.attributes[k] = v.dump();
                        }
                    }
                }

                if (e.header.uuid.empty()) {
                    if (!e.payload.statement.empty()) {
                        uint32_t h = std::hash<std::string>{}(e.payload.statement);
                        char hex[9];
                        snprintf(hex, sizeof(hex), "%08x", h);
                        e.header.uuid = "atom-" + std::string(hex);
                    } else {
                        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        char hex[17];
                        snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(now_us));
                        e.header.uuid = "atom-" + std::string(hex);
                    }
                }

                std::cout << "[API] Processing Atom Statement: " << e.payload.statement << std::endl;
                if (blackboard_->commit_cpb_entry(e, principal_id)) {
                    count++;
                    if (!e.header.origin.context_id.empty()) {
                        json atom_evt = {
                            {"context_id", e.header.origin.context_id},
                            {"uuid", e.header.uuid}
                        };
                        context_broker_.broadcast(e.header.origin.context_id, "atom_committed", atom_evt.dump());
                    }
                }
            }

            if (count == 0) {
                res.status = 400;
                res.set_content("Error: Failed to commit any anchored nodes.", "text/plain");
                return;
            }
            
            res.status = 200;
            res.set_content("Successfully committed " + std::to_string(count) + " anchored nodes.", "text/plain");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 3. Robust Querying API
    // Expects Query DSL: { "match": "...", "where_eq": {"key": "val"} }
    svr.Post("/api/v1/query", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            auto j = json::parse(req.body);
            auto engine = blackboard_->get_engine();

            auto q = engine->query();
            q.set_principal_id(principal_id);

            std::string alias = "n";
            if (j.contains("match")) {
                alias = j["match"];
                q.match(alias);
            }

            if (j.contains("where_eq")) {
                for (auto& [key, val] : j["where_eq"].items()) {
                    q.where_eq(alias, key, val.get<std::string>());
                }
            }

            auto results = q.execute();
            json response = json::array();
            for (auto& row : results) {
                response.push_back(row.fields);
            }
            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 2b. Manual Synapse Creation (Link)
    // { "source": "...", "target": "...", "label": "...", "weight": 1.0 }
    svr.Post("/api/v1/link", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            auto j = json::parse(req.body);
            std::string src = j.at("source");
            std::string dst = j.at("target");
            std::string label = j.at("label");
            double weight = j.value("weight", 1.0);

            auto engine = blackboard_->get_engine();
            engine->add_edge(src, label, weight, dst);

            res.set_content("{\"status\":\"SYNCED\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });
 
 
    // 2c. Atom Promotion (Move to PRINCIPLE status)
    svr.Post("/api/v1/cpb/promote", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            auto j = json::parse(req.body);
            std::string uuid = j.at("uuid");
            
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();
            
            auto key_uuid = engine->get_resolver().parse_uuid(uuid);
            std::string key = std::string(l3kvg::KeyBuilder::node_key(key_uuid));
            auto buf = store->get(key, principal_id);
            if (buf.size() == 0) {
                res.status = 404;
                res.set_content("Atom not found", "text/plain");
                return;
            }
            
            CpbEntry e = CpbEntry::deserialize(buf);
            e.taxonomy.is_principle = true; // Promoted state
            e.taxonomy.uncertainty = false; // Promotion implies validation
            
            if (blackboard_->commit_cpb_entry(e, principal_id)) {
                if (!e.header.origin.context_id.empty()) {
                    json atom_evt = {
                        {"context_id", e.header.origin.context_id},
                        {"uuid", e.header.uuid}
                    };
                    context_broker_.broadcast(e.header.origin.context_id, "atom_committed", atom_evt.dump());
                }
                res.set_content("{\"status\":\"PROMOTED\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("Failed to update status", "text/plain");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 5. Nucleus Integration Endpoints
    svr.Post("/api/v1/nucleus/materialize", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            auto j = json::parse(req.body);
            std::string atom_id = j.at("atom_id");
            std::string behavior = j.at("behavior");
            std::string label = j.value("label", "");

            // Build Nucleus MCP Request
            json nucleus_req = {
                {"method", "spawn_fragment"},
                {"params", {
                    {"atom_id", atom_id},
                    {"behavior", behavior},
                    {"label", label}
                }}
            };

            agentic_blackboard::Orchestrator::instance().dispatch_nucleus_command(nucleus_req.dump());
            res.set_content("{\"status\":\"DISPATCHED\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });

    svr.Post("/api/v1/nucleus/bind", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            auto j = json::parse(req.body);
            std::string marker_id = j.at("marker_id");
            std::string atom_id = j.at("atom_id");

            json nucleus_req = {
                {"method", "kv_set"},
                {"params", {
                    {"key", "anchor:" + marker_id},
                    {"value", atom_id}
                }}
            };

            agentic_blackboard::Orchestrator::instance().dispatch_nucleus_command(nucleus_req.dump());
            res.set_content("{\"status\":\"BOUND\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 4a. Commonplace Search API
    svr.Get("/api/v1/search", [this](const httplib::Request& req, httplib::Response& res) {
        std::cout << "[API] GET /api/v1/search" << std::endl;
        try {
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();

            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string q = req.get_param_value("q");
            auto to_lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                return s;
            };
            std::string q_lower = to_lower(q);

            bool filter_ka = req.has_param("ka");
            int ka_val = 0;
            if (filter_ka) {
                try { ka_val = std::stoi(req.get_param_value("ka")); } catch (...) { filter_ka = false; }
            }

            bool filter_tags = req.has_param("tags") && !req.get_param_value("tags").empty();
            std::vector<std::string> requested_tags;
            if (filter_tags) {
                std::string tag_str = req.get_param_value("tags");
                std::stringstream ss(tag_str);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    size_t first = item.find_first_not_of(" \t\r\n");
                    if (first != std::string::npos) {
                        size_t last = item.find_last_not_of(" \t\r\n");
                        requested_tags.push_back(to_lower(item.substr(first, last - first + 1)));
                    }
                }
                if (requested_tags.empty()) filter_tags = false;
            }

            int limit = 10;
            if (req.has_param("limit")) {
                try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
            }

            std::set<std::string> unique_keys;
            auto keys = store->get_prefix_keys_all_shards("n:", "n:", engine->get_settings().prefix_scan_limit);
            unique_keys.insert(keys.begin(), keys.end());

            json matches = json::array();
            std::unordered_set<std::string> seen_uuids;

            for (const auto& key : unique_keys) {
                if (key.ends_with(":meta")) continue;

                auto buf = store->get(key, principal_id);
                if (buf.size() == 0) continue;

                try {
                    size_t h_idx = buf.get_obj(0, "header");
                    bool has_type = false;
                    std::string type = "";
                    try {
                        type = std::string(buf.get_str(h_idx, "type"));
                        has_type = true;
                    } catch (...) {}

                    if (has_type && (type == "IDENTITY" || type == "PROJECT" || type == "EU" || type == "WBS_NODE" || type == "L3_DELTA_PATCH")) {
                        continue;
                    }

                    auto entry = CpbEntry::deserialize(buf);
                    if (entry.header.uuid.empty()) continue;
                    if (seen_uuids.count(entry.header.uuid) > 0) continue;

                    // Filter KA
                    if (filter_ka && static_cast<int>(entry.taxonomy.knowledge_area) != ka_val) {
                        continue;
                    }

                    // Filter Tags
                    if (filter_tags) {
                        bool any_tag_matched = false;
                        for (const auto& req_tag : requested_tags) {
                            for (const auto& at : entry.taxonomy.tags) {
                                if (to_lower(at) == req_tag) {
                                    any_tag_matched = true;
                                    break;
                                }
                            }
                            if (any_tag_matched) break;
                        }
                        if (!any_tag_matched) continue;
                    }

                    // Filter query string q
                    if (!q_lower.empty()) {
                        bool statement_match = to_lower(entry.payload.statement).find(q_lower) != std::string::npos;
                        bool content_match = to_lower(entry.payload.content).find(q_lower) != std::string::npos;
                        bool tag_match = false;
                        for (const auto& at : entry.taxonomy.tags) {
                            if (to_lower(at).find(q_lower) != std::string::npos) {
                                tag_match = true;
                                break;
                            }
                        }
                        if (!statement_match && !content_match && !tag_match) {
                            continue;
                        }
                    }

                    seen_uuids.insert(entry.header.uuid);

                    json match_obj = {
                        {"uuid", entry.header.uuid},
                        {"id", entry.header.uuid},
                        {"statement", entry.payload.statement},
                        {"content", entry.payload.content},
                        {"ka", static_cast<int>(entry.taxonomy.knowledge_area)},
                        {"tags", entry.taxonomy.tags},
                        {"author", entry.header.origin.agent_id},
                        {"project", entry.header.origin.project_id},
                        {"score", 1.0}
                    };
                    matches.push_back(match_obj);

                    if (limit > 0 && static_cast<int>(matches.size()) >= limit) {
                        break;
                    }
                } catch (...) {}
            }

            json response = {
                {"matches", matches},
                {"count", matches.size()}
            };
            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            std::cerr << "[API] Error in search: " << e.what() << std::endl;
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 4. Graph Topology Snapshot
    svr.Get("/api/v1/graph/snapshot", [this](const httplib::Request& req, httplib::Response& res) {
        std::cout << "[API] GET /api/v1/graph/snapshot" << std::endl;
        try {
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();

            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }
            
            json response;
            response["nodes"] = json::array();
            response["edges"] = json::array();

            // 1. Fetch ALL Nodes (Atoms, Identities, Projects)
            std::set<std::string> unique_keys;
            auto keys1 = store->get_prefix_keys_all_shards("n:", "n:", 1000);
            auto keys2 = store->get_prefix_keys_all_shards("n:{", "n:{", 1000);
            unique_keys.insert(keys1.begin(), keys1.end());
            unique_keys.insert(keys2.begin(), keys2.end());

            std::cout << "[API] Snapshot Scan: Found " << unique_keys.size() << " potential node keys." << std::endl;

            std::unordered_set<std::string> materialized_ids;

            for(const auto& key : unique_keys) {
                auto buf = store->get(key, principal_id);
                if (buf.size() > 0) {
                    try {
                        size_t h_idx = buf.get_obj(0, "header");
                        bool has_type = false;
                        std::string type = "ATOM";

                        try {
                            type = std::string(buf.get_str(h_idx, "type"));
                            has_type = true;
                        } catch (...) {}

                        if (has_type && type == "IDENTITY") {
                            auto iden = IdentityNode::deserialize(buf);
                            response["nodes"].push_back({
                                {"id", iden.id},
                                {"type", "IDENTITY"},
                                {"label", iden.display_name},
                                {"role", iden.role},
                                {"content", iden.content}
                            });
                            materialized_ids.insert(iden.id);
                            std::cout << "[API] Materialized IDENTITY: " << iden.id << std::endl;
                        } else if (has_type && type == "PROJECT") {
                            auto proj = ProjectNode::deserialize(buf);
                            response["nodes"].push_back({
                                {"id", proj.project_id},
                                {"type", "PROJECT"},
                                {"label", proj.project_id},
                                {"status", proj.lifecycle_status},
                                {"content", proj.content}
                            });
                            materialized_ids.insert(proj.project_id);
                            std::cout << "[API] Materialized PROJECT: " << proj.project_id << std::endl;
                        } else {
                            // Default: Knowledge Atom
                            auto entry = CpbEntry::deserialize(buf);
                            json node_json = {
                                {"id", entry.header.uuid},
                                {"type", "ATOM"},
                                {"label", entry.payload.statement},
                                {"statement", entry.payload.statement},
                                {"content", entry.payload.content},
                                {"project", entry.header.origin.project_id},
                                {"author", entry.header.origin.agent_id},
                                {"ka", static_cast<int>(entry.taxonomy.knowledge_area)},
                                {"tags", entry.taxonomy.tags}
                            };
                            if (entry.wellness.has_value()) {
                                node_json["wellness"] = {
                                    {"mood_sentiment", entry.wellness->mood_sentiment},
                                    {"energy_level", entry.wellness->energy_level},
                                    {"sleep_hours", entry.wellness->sleep_hours},
                                    {"active_minutes", entry.wellness->active_minutes},
                                    {"step_count", entry.wellness->step_count},
                                    {"activity_type", entry.wellness->activity_type}
                                };
                            }
                            if (entry.education.has_value()) {
                                node_json["education"] = {
                                    {"institution_platform", entry.education->institution_platform},
                                    {"resource_type", entry.education->resource_type},
                                    {"progress_percent", entry.education->progress_percent},
                                    {"focus_duration_minutes", entry.education->focus_duration_minutes},
                                    {"credential_uuid", entry.education->credential_uuid}
                                };
                            }
                            response["nodes"].push_back(node_json);
                            materialized_ids.insert(entry.header.uuid);
                        }
                    } catch (...) {}
                }
            }


            // 2. Fetch Edges
            auto edge_keys = store->get_prefix_keys_all_shards("e:out:{", "e:out:{", 2000);
            std::cout << "[API] Snapshot Scan: Found " << edge_keys.size() << " potential synapses." << std::endl;

            for(const auto& key : edge_keys) {
                // Key format: e:out:{src}:{label}:{weight}:{dst}
                try {
                    size_t src_start = key.find("{") + 1;
                    size_t src_end = key.find("}", src_start);
                    std::string src = key.substr(src_start, src_end - src_start);

                    size_t label_start = src_end + 2;
                    size_t label_end = key.find(":", label_start);
                    std::string label = key.substr(label_start, label_end - label_start);

                    size_t weight_start = label_end + 1;
                    size_t weight_end = key.find(":", weight_start);
                    double weight = std::stod(key.substr(weight_start, weight_end - weight_start));

                    // Destination is everything after the weight_end + 1 (the colon)
                    std::string dst = key.substr(weight_end + 1);

                    // Fallback: If destination node is not in materialized_ids, try to fetch it directly (only if we have read permission on it)
                    if (materialized_ids.find(dst) == materialized_ids.end()) {
                        std::string dst_node_key = std::string(l3kvg::KeyBuilder::node_key(blackboard_->get_engine()->get_resolver().parse_uuid(dst)));
                        auto dst_buf = store->get(dst_node_key, principal_id);
                        if (dst_buf.size() > 0) {
                            std::cout << "[API] Fallback Retrieval for Target: " << dst << std::endl;
                            try {
                                size_t h_idx = dst_buf.get_obj(0, "header");
                                std::string t = std::string(dst_buf.get_str(h_idx, "type"));
                                if (t == "IDENTITY") {
                                    auto iden = IdentityNode::deserialize(dst_buf);
                                    response["nodes"].push_back({{"id", iden.id}, {"type", "IDENTITY"}, {"label", iden.display_name}, {"role", iden.role}});
                                } else if (t == "PROJECT") {
                                    auto proj = ProjectNode::deserialize(dst_buf);
                                    response["nodes"].push_back({{"id", proj.project_id}, {"type", "PROJECT"}, {"label", proj.project_id}, {"status", proj.lifecycle_status}});
                                }
                                materialized_ids.insert(dst);
                            } catch (...) {}
                        }
                    }

                    if (materialized_ids.find(src) != materialized_ids.end() &&
                        materialized_ids.find(dst) != materialized_ids.end()) {
                        response["edges"].push_back({
                            {"source", src},
                            {"target", dst},
                            {"label", label},
                            {"weight", weight}
                        });
                    }
                } catch (...) {}
            }


            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            std::cerr << "[API] Error generating graph snapshot: " << e.what() << std::endl;
            json err = {{"error", e.what()}, {"nodes", json::array()}, {"edges", json::array()}};
            res.status = 500;
            res.set_content(err.dump(2), "application/json");
        }
    });

    // 4b. RDF Turtle Export Endpoint
    svr.Get("/api/v1/graph/export", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string format = req.get_param_value("format");
            std::string format_lower = format;
            std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            if (format_lower.empty() || format_lower == "turtle" || format_lower == "ttl" || format_lower == "text/turtle") {
                std::string ttl = RdfExporter::export_turtle(blackboard_, principal_id);
                res.set_content(ttl, "text/turtle");
            } else {
                res.status = 400;
                res.set_content("Unsupported format: " + format, "text/plain");
            }
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 5. Idempotent Node Creation (For MCP)
    svr.Post("/api/v1/graph/node", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            auto j = json::parse(req.body);
            std::string type = j.at("type");
            std::string id = j.at("id");
            json metadata = j.value("metadata", json::object());

            if (auth_role != "admin" && type == "IDENTITY") {
                std::string pure_active = pure_user_id(active_user);
                std::string pure_node_id = pure_user_id(id);
                bool allowed = (pure_node_id == pure_active) ||
                               (pure_node_id == pure_active + "-agent") ||
                               (id == "identity:" + pure_active) ||
                               (id == "user:" + pure_active) ||
                               (id == "agent:" + pure_active + "-agent");
                if (!allowed) {
                    res.status = 403;
                    res.set_content(json({{"error", "Forbidden"}, {"message", "Cannot create IDENTITY node for another user"}}).dump(), "application/json");
                    return;
                }
                if (metadata.contains("user_id")) {
                    std::string meta_user = metadata.value("user_id", "");
                    if (!meta_user.empty() && pure_user_id(meta_user) != pure_active) {
                        res.status = 403;
                        res.set_content(json({{"error", "Forbidden"}, {"message", "Cannot create IDENTITY node for another user"}}).dump(), "application/json");
                        return;
                    }
                }
            }

            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();

            std::string key = std::string(l3kvg::KeyBuilder::node_key(blackboard_->get_engine()->get_resolver().parse_uuid(id)));
            auto buf = store->get(key, principal_id);

            if (buf.size() > 0) {
                res.status = 200;
                res.set_content("{\"status\":\"EXISTS\", \"id\":\"" + id + "\"}", "application/json");
                return;
            }

            bool success = false;
            if (type == "PROJECT") {
                ProjectNode p = {id, metadata.value("description", ""), metadata.value("status", "ACTIVE"), metadata.value("content", "")};
                success = blackboard_->commit_project_node(p);
            } else if (type == "IDENTITY") {
                IdentityNode i = {id, metadata.value("name", id), metadata.value("role", "AGENT"), metadata.value("pubkey", ""), metadata.value("content", "")};
                success = blackboard_->commit_identity_node(i);
            }

            if (success) {
                // Seed write/read ACL permission for the creator
                if (principal_id != 0) {
                    store->credentials().set_acl(principal_id, key, l3kv::Permission::READ | l3kv::Permission::WRITE);
                    std::string hex_part = key.substr(2); // "{hex_id}"
                    store->credentials().set_acl(principal_id, "e:out:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                    store->credentials().set_acl(principal_id, "e:in:" + hex_part, l3kv::Permission::READ | l3kv::Permission::WRITE);
                }
                res.status = 201;
                res.set_content("{\"status\":\"CREATED\", \"id\":\"" + id + "\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{\"error\":\"Failed to commit node\"}", "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 5b. Get Node Links (Inbound & Outbound)
    svr.Get(R"(/api/v1/node/([^/]+)/links)", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string uuid = req.matches[1];
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();

            std::string db_key = std::string(l3kvg::KeyBuilder::node_key(engine->get_resolver().parse_uuid(uuid)));
            auto node_buf = store->get(db_key, principal_id);
            if (node_buf.size() == 0) {
                res.status = 404;
                res.set_content("Node not found", "text/plain");
                return;
            }

            std::string direction = req.get_param_value("direction");
            if (direction.empty()) direction = "both";

            auto hydrate_statement = [&](const std::string& target_id) -> std::string {
                try {
                    auto target_key = std::string(l3kvg::KeyBuilder::node_key(engine->get_resolver().parse_uuid(target_id)));
                    auto target_buf = store->get(target_key, principal_id);
                    if (target_buf.size() > 0) {
                        try {
                            size_t p_idx = target_buf.get_obj(0, "payload");
                            return std::string(target_buf.get_str(p_idx, "statement"));
                        } catch (...) {
                            try {
                                return std::string(target_buf.get_str(0, "display_name"));
                            } catch (...) {
                                try {
                                    return std::string(target_buf.get_str(0, "description"));
                                } catch (...) {}
                            }
                        }
                    }
                } catch (...) {}
                return "";
            };

            json inbound_arr = json::array();
            if (direction == "both" || direction == "inbound") {
                auto backlinks = blackboard_->get_backlinks(uuid, principal_id);
                for (const auto& [src, rel_name] : backlinks) {
                    inbound_arr.push_back({
                        {"uuid", src},
                        {"source", src},
                        {"relation", rel_name},
                        {"statement", hydrate_statement(src)}
                    });
                }
            }

            json outbound_arr = json::array();
            if (direction == "both" || direction == "outbound") {
                auto outbound = blackboard_->get_outbound_links(uuid, principal_id);
                for (const auto& [dst, rel_name] : outbound) {
                    outbound_arr.push_back({
                        {"uuid", dst},
                        {"target", dst},
                        {"relation", rel_name},
                        {"statement", hydrate_statement(dst)}
                    });
                }
            }

            json response = {
                {"uuid", uuid},
                {"inbound", inbound_arr},
                {"outbound", outbound_arr}
            };
            res.set_content(response.dump(2), "application/json");
        } catch (const std::exception& e) {
            std::cerr << "[API] Error in get_node_links: " << e.what() << std::endl;
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 6. Get Node Details
    svr.Get(R"(/api/v1/node/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string uuid = req.matches[1];
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();

            std::string key = std::string(l3kvg::KeyBuilder::node_key(blackboard_->get_engine()->get_resolver().parse_uuid(uuid)));
            auto buf = store->get(key, principal_id);

            if (buf.size() == 0) {
                res.status = 404;
                res.set_content("Node not found", "text/plain");
                return;
            }

            size_t h_idx = buf.get_obj(0, "header");
            bool has_type = false;
            std::string type = "ATOM";
            try {
                type = std::string(buf.get_str(h_idx, "type"));
                has_type = true;
            } catch (...) {}

            if (has_type && type == "IDENTITY") {
                auto iden = IdentityNode::deserialize(buf);
                json node_json = {
                    {"id", iden.id},
                    {"uuid", iden.id},
                    {"type", "IDENTITY"},
                    {"label", iden.display_name},
                    {"display_name", iden.display_name},
                    {"role", iden.role},
                    {"public_key", iden.public_key},
                    {"content", iden.content},
                    {"found", true}
                };
                res.set_content(node_json.dump(2), "application/json");
            } else if (has_type && type == "PROJECT") {
                auto proj = ProjectNode::deserialize(buf);
                json node_json = {
                    {"id", proj.project_id},
                    {"uuid", proj.project_id},
                    {"project_id", proj.project_id},
                    {"type", "PROJECT"},
                    {"label", proj.project_id},
                    {"status", proj.lifecycle_status},
                    {"lifecycle_status", proj.lifecycle_status},
                    {"description", proj.description},
                    {"content", proj.content},
                    {"found", true}
                };
                res.set_content(node_json.dump(2), "application/json");
            } else {
                auto entry = CpbEntry::deserialize(buf);
                json node_json = {
                    {"id", entry.header.uuid},
                    {"uuid", entry.header.uuid},
                    {"type", "ATOM"},
                    {"label", entry.payload.statement},
                    {"statement", entry.payload.statement},
                    {"content", entry.payload.content},
                    {"project", entry.header.origin.project_id},
                    {"author", entry.header.origin.agent_id},
                    {"user_id", entry.header.origin.user_id},
                    {"agent_id", entry.header.origin.agent_id},
                    {"surface_id", entry.header.origin.surface_id},
                    {"surface_type", entry.header.origin.surface_type},
                    {"context_id", entry.header.origin.context_id},
                    {"origin", {
                        {"user_id", entry.header.origin.user_id},
                        {"agent_id", entry.header.origin.agent_id},
                        {"surface_id", entry.header.origin.surface_id},
                        {"surface_type", entry.header.origin.surface_type},
                        {"context_id", entry.header.origin.context_id},
                        {"project_id", entry.header.origin.project_id}
                    }},
                    {"header", {
                        {"uuid", entry.header.uuid},
                        {"origin", {
                            {"user_id", entry.header.origin.user_id},
                            {"agent_id", entry.header.origin.agent_id},
                            {"surface_id", entry.header.origin.surface_id},
                            {"surface_type", entry.header.origin.surface_type},
                            {"context_id", entry.header.origin.context_id},
                            {"project_id", entry.header.origin.project_id}
                        }}
                    }},
                    {"ka", static_cast<int>(entry.taxonomy.knowledge_area)},
                    {"tags", entry.taxonomy.tags},
                    {"applicability", entry.taxonomy.applicability},
                    {"uncertainty", entry.taxonomy.uncertainty},
                    {"is_principle", entry.taxonomy.is_principle},
                    {"found", true}
                };

                if (!entry.payload.references.empty()) {
                    json refs_arr = json::array();
                    for (const auto& r : entry.payload.references) {
                        refs_arr.push_back({
                            {"title", r.title},
                            {"page_numbers", r.page_numbers},
                            {"uuid", r.uuid},
                            {"creator", r.creator},
                            {"tags", r.tags},
                            {"excerpt", r.excerpt}
                        });
                    }
                    node_json["references"] = refs_arr;
                }

                if (!entry.payload.note_links.empty()) {
                    json links_arr = json::array();
                    for (const auto& l : entry.payload.note_links) {
                        links_arr.push_back({
                            {"target_uuid", l.target_uuid},
                            {"relation", l.relation},
                            {"context", l.context}
                        });
                    }
                    node_json["note_links"] = links_arr;
                }

                if (!entry.items.empty()) {
                    json items_arr = json::array();
                    for (const auto& item : entry.items) {
                        items_arr.push_back({
                            {"name", item.name},
                            {"quantity", item.quantity},
                            {"unit", item.unit},
                            {"role", item.role},
                            {"notes", item.notes}
                        });
                    }
                    node_json["items"] = items_arr;
                }

                if (!entry.steps.empty()) {
                    json steps_arr = json::array();
                    for (const auto& step : entry.steps) {
                        steps_arr.push_back({
                            {"step_number", step.step_number},
                            {"instruction", step.instruction},
                            {"duration_seconds", step.duration_seconds},
                            {"notes", step.notes}
                        });
                    }
                    node_json["steps"] = steps_arr;
                }

                if (!entry.metrics.empty()) {
                    json metrics_arr = json::array();
                    for (const auto& metric : entry.metrics) {
                        metrics_arr.push_back({
                            {"name", metric.key},
                            {"key", metric.key},
                            {"value", metric.value},
                            {"unit", metric.unit}
                        });
                    }
                    node_json["metrics"] = metrics_arr;
                }

                if (!entry.attributes.empty()) {
                    node_json["attributes"] = entry.attributes;
                }

                if (entry.wellness.has_value()) {
                    node_json["wellness"] = {
                        {"mood_sentiment", entry.wellness->mood_sentiment},
                        {"energy_level", entry.wellness->energy_level},
                        {"sleep_hours", entry.wellness->sleep_hours},
                        {"active_minutes", entry.wellness->active_minutes},
                        {"step_count", entry.wellness->step_count},
                        {"activity_type", entry.wellness->activity_type}
                    };
                }

                if (entry.education.has_value()) {
                    node_json["education"] = {
                        {"institution_platform", entry.education->institution_platform},
                        {"resource_type", entry.education->resource_type},
                        {"progress_percent", entry.education->progress_percent},
                        {"focus_duration_minutes", entry.education->focus_duration_minutes},
                        {"credential_uuid", entry.education->credential_uuid}
                    };
                }

                res.set_content(node_json.dump(2), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 7. SRE Health Metrics
    svr.Get("/api/v1/health", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto engine = blackboard_->get_engine();
            auto store = engine->get_store();
            auto buf = store->get("n:{governance:swarm_health}");
            
            if (buf.size() == 0) {
                res.status = 404;
                res.set_content("{\"error\":\"Health substrate not initialized\"}", "application/json");
                return;
            }

            auto health = SwarmHealthSummary::deserialize(buf);
            json j = {
                {"status", "OPERATIONAL"},
                {"metrics", {
                    {"knowledge_velocity", health.metrics.knowledge_velocity},
                    {"sync_latency_ms", health.metrics.sync_latency_ms},
                    {"toil_ratio", health.metrics.toil_ratio}
                }},
                {"cluster", health.cluster_status}
            };
            res.set_content(j.dump(2), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 7b. Admin User & Token Management
    svr.Post("/api/v1/admin/users", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            if (auth_role != "admin") {
                res.status = 403;
                res.set_content(json({{"error", "Forbidden"}, {"message", "Admin role required"}}).dump(), "application/json");
                return;
            }

            auto j = json::parse(req.body);
            std::string username = j.value("username", "");
            std::string role = j.value("role", "curator");
            std::string token = j.value("token", "");

            if (username.empty() || token.empty()) {
                res.status = 400;
                res.set_content(json({{"error", "Bad Request"}, {"message", "Missing username or token"}}).dump(), "application/json");
                return;
            }

            if (!blackboard_->register_token(token, username, role)) {
                res.status = 500;
                res.set_content(json({{"error", "Internal Error"}, {"message", "Failed to register token"}}).dump(), "application/json");
                return;
            }

            json resp = {
                {"status", "CREATED"},
                {"username", username},
                {"role", role},
                {"token", token}
            };
            res.status = 200;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    svr.Get("/api/v1/admin/users", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            if (auth_role != "admin") {
                res.status = 403;
                res.set_content(json({{"error", "Forbidden"}, {"message", "Admin role required"}}).dump(), "application/json");
                return;
            }

            auto users = blackboard_->get_registered_users();
            json users_arr = json::array();
            for (const auto& u : users) {
                users_arr.push_back({
                    {"username", u.first},
                    {"role", u.second}
                });
            }

            json resp = {
                {"status", "OK"},
                {"users", users_arr}
            };
            res.status = 200;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    });

    // 8. Shared Workspace Context & Multi-Surface SSE Event Sync
    svr.Post("/api/v1/context/register", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            auto j = json::parse(req.body);
            std::string context_id = j.value("context_id", "");
            std::string surface_id = j.value("surface_id", "");
            std::string client_app = j.value("client_app", "");
            json capabilities = j.value("capabilities", json::object());

            if (context_id.empty() || surface_id.empty()) {
                res.status = 400;
                res.set_content(json({{"error", "Missing context_id or surface_id"}}).dump(), "application/json");
                return;
            }

            json resp;
            context_broker_.register_surface(context_id, surface_id, client_app, capabilities, resp);
            res.status = 200;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.Get(R"(/api/v1/context/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string context_id = req.matches[1];
            json state;
            if (!context_broker_.get_context_state(context_id, state)) {
                res.status = 404;
                res.set_content(json({{"error", "Context not found"}, {"context_id", context_id}}).dump(), "application/json");
                return;
            }

            res.status = 200;
            res.set_content(state.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.Post(R"(/api/v1/context/([^/]+)/focus)", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string context_id = req.matches[1];
            auto j = json::parse(req.body);
            std::string surface_id = j.value("surface_id", "");

            json broadcast_payload;
            context_broker_.update_focus(context_id, surface_id, j, broadcast_payload);

            res.status = 200;
            res.set_content(json({{"status", "OK"}, {"context_id", context_id}}).dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.Get("/api/v1/events", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string context_id;
            if (req.has_param("context")) {
                context_id = req.get_param_value("context");
            }

            if (context_id.empty()) {
                res.status = 400;
                res.set_content(json({{"error", "Missing context query parameter"}}).dump(), "application/json");
                return;
            }

            auto session = context_broker_.create_client(context_id);

            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");

            res.set_chunked_content_provider(
                "text/event-stream",
                [session, this](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    if (!running_ || !session->active) {
                        return false;
                    }
                    if (!sink.is_writable()) {
                        return false;
                    }

                    std::vector<std::string> msgs;
                    {
                        std::unique_lock<std::mutex> lock(session->mutex);
                        session->cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                            return !running_ || !session->active || !session->event_queue.empty();
                        });

                        if (!running_ || !session->active) {
                            return false;
                        }
                        while (!session->event_queue.empty()) {
                            msgs.push_back(std::move(session->event_queue.front()));
                            session->event_queue.pop();
                        }
                    }

                    if (msgs.empty()) {
                        std::string keepalive = ": keepalive\n\n";
                        return sink.write(keepalive.data(), keepalive.size());
                    }

                    for (const auto& msg : msgs) {
                        if (!sink.write(msg.data(), msg.size())) {
                            return false;
                        }
                    }
                    return true;
                },
                [session, this](bool /*success*/) {
                    session->active = false;
                    session->cv.notify_all();
                    context_broker_.remove_client(session->context_id, session->id);
                }
            );
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    // 9. Surface Pairing & Scoped Token Management
    svr.Post("/api/v1/surface/pair/request", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            json j = json::object();
            if (!req.body.empty()) {
                j = json::parse(req.body);
            }
            std::string surface_type = j.value("surface_type", "unknown");
            std::string client_app = j.value("client_app", "");
            std::string suggested_id = j.value("suggested_id", "");

            json resp;
            if (!context_broker_.request_surface_pairing(surface_type, client_app, suggested_id, resp)) {
                res.status = 400;
                res.set_content(resp.dump(), "application/json");
                return;
            }

            res.status = 201;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.Post("/api/v1/surface/pair/approve", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            auto j = json::parse(req.body);
            std::string pairing_id = j.value("pairing_id", "");
            std::string pin = j.value("pin", "");
            std::string user_id = j.value("user_id", active_user);
            std::string agent_id = j.value("agent_id", "");
            std::string context_id = j.value("context_id", "");
            std::string surface_id = j.value("surface_id", "");

            if (pairing_id.empty() || pin.empty()) {
                res.status = 400;
                res.set_content(json({{"error", "Missing pairing_id or pin"}}).dump(), "application/json");
                return;
            }

            if (auth_role != "admin" && pure_user_id(user_id) != pure_user_id(active_user)) {
                res.status = 403;
                res.set_content(json({{"error", "Forbidden"}, {"message", "Cannot approve pairing for another user"}}).dump(), "application/json");
                return;
            }

            if (auth_role != "admin") {
                std::string expected_agent = "agent:" + pure_user_id(active_user) + "-agent";
                if (agent_id.empty()) {
                    agent_id = expected_agent;
                } else {
                    std::string pure_agent = (agent_id.rfind("agent:", 0) == 0) ? agent_id.substr(6) : agent_id;
                    std::string u = pure_user_id(active_user);
                    if (agent_id != expected_agent && pure_agent.find(u) == std::string::npos) {
                        res.status = 403;
                        res.set_content(json({{"error", "Non-admin cannot bind surface to another user's agent"}}).dump(), "application/json");
                        return;
                    }
                }
            }

            json resp;
            if (!context_broker_.approve_surface_pairing(pairing_id, pin, user_id, agent_id, context_id, surface_id, resp)) {
                res.status = 400;
                res.set_content(resp.dump(), "application/json");
                return;
            }

            res.status = 200;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.Post("/api/v1/surface/pair/claim", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            std::string pairing_id = j.value("pairing_id", "");
            if (pairing_id.empty()) {
                res.status = 400;
                res.set_content(json({{"error", "Missing pairing_id"}}).dump(), "application/json");
                return;
            }

            json resp;
            if (!context_broker_.claim_surface_pairing(pairing_id, resp)) {
                std::string err = resp.value("error", "");
                if (err == "Pairing session not found") {
                    res.status = 404;
                } else {
                    res.status = 400;
                }
                res.set_content(resp.dump(), "application/json");
                return;
            }

            res.status = 200;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.Get("/api/v1/surface/list", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string filter_user = (auth_role == "admin") ? "" : active_user;
            json resp;
            context_broker_.list_enrolled_surfaces(filter_user, resp);
            res.status = 200;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    svr.Delete(R"(/api/v1/surface/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            std::string active_user;
            std::string auth_role;
            uint32_t principal_id = 0;
            if (!authenticate_request(req, res, principal_id, active_user, auth_role)) {
                return;
            }

            std::string surface_id = req.matches[1];
            if (auth_role != "admin") {
                EnrolledSurface s;
                if (context_broker_.get_enrolled_surface(surface_id, s)) {
                    if (pure_user_id(s.user_id) != pure_user_id(active_user)) {
                        res.status = 403;
                        res.set_content(json({{"error", "Forbidden"}, {"message", "Cannot revoke another user's surface"}}).dump(), "application/json");
                        return;
                    }
                }
            }

            json resp;
            if (!context_broker_.revoke_enrolled_surface(surface_id, resp)) {
                res.status = 404;
                res.set_content(resp.dump(), "application/json");
                return;
            }

            res.status = 200;
            res.set_content(resp.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    std::cout << "[API] Agentic Blackboard Server starting on " << host_ << ":" << port_ << "..." << std::endl;

    if (!svr.listen(host_.c_str(), port_)) {
        std::cerr << "[API] FAILED to start server on " << host_ << ":" << port_ << std::endl;
    }
    s_server.store(nullptr);
}


} // namespace agentic_blackboard

#pragma once

#include <agentic_blackboard/Blackboard.hpp>
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace httplib {
class Request;
class Response;
}

namespace agentic_blackboard {

struct SurfaceInfo {
    std::string surface_id;
    std::string client_app;
    nlohmann::json capabilities;
    int64_t registered_at{0};
};

struct ContextRecord {
    std::string context_id;
    std::unordered_map<std::string, SurfaceInfo> surfaces;
    nlohmann::json focus = nlohmann::json::object({{"selected", nlohmann::json::array()}});
};

struct PairingSession {
    std::string pairing_id;
    std::string pin;
    std::string surface_type;
    std::string client_app;
    std::string suggested_id;
    int64_t created_at_sec{0};
    int64_t expires_at_sec{0};
    int failed_attempts{0};
    bool approved{false};
    std::string approved_user_id;
    std::string approved_agent_id;
    std::string assigned_surface_id;
    std::string assigned_context_id;
    std::string surface_token;
    bool claimed{false};
};

struct EnrolledSurface {
    std::string surface_id;
    std::string surface_type;
    std::string client_app;
    std::string user_id;
    std::string agent_id;
    std::string context_id;
    std::string token_hash;
    int64_t enrolled_at_sec{0};
    int64_t last_seen_sec{0};
};

struct SseClientSession {
    static constexpr size_t kMaxQueueSize = 1000;
    uint64_t id{0};
    std::string context_id;
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::string> event_queue;
    std::atomic<bool> active{true};
};

class ContextBroker {
public:
    ContextBroker() = default;

    void set_blackboard(Blackboard* bb);

    bool register_surface(const std::string& context_id,
                          const std::string& surface_id,
                          const std::string& client_app,
                          const nlohmann::json& capabilities,
                          nlohmann::json& out_resp);

    bool get_context_state(const std::string& context_id, nlohmann::json& out_state);

    bool update_focus(const std::string& context_id,
                      const std::string& surface_id,
                      const nlohmann::json& focus_payload,
                      nlohmann::json& out_broadcast_payload);

    std::shared_ptr<SseClientSession> create_client(const std::string& context_id);
    void remove_client(const std::string& context_id, uint64_t client_id);
    void broadcast(const std::string& context_id, const std::string& event_name, const std::string& json_data);
    void shutdown();

    bool request_surface_pairing(const std::string& surface_type,
                                const std::string& client_app,
                                const std::string& suggested_id,
                                nlohmann::json& out_resp);

    bool approve_surface_pairing(const std::string& pairing_id,
                                const std::string& pin,
                                const std::string& user_id,
                                const std::string& agent_id,
                                const std::string& context_id,
                                const std::string& surface_id,
                                nlohmann::json& out_resp);

    bool claim_surface_pairing(const std::string& pairing_id,
                              nlohmann::json& out_resp);

    bool list_enrolled_surfaces(const std::string& user_id, nlohmann::json& out_resp);

    bool revoke_enrolled_surface(const std::string& surface_id, nlohmann::json& out_resp);

    bool get_enrolled_surface(const std::string& surface_id, EnrolledSurface& out_surface);

    void touch_surface(const std::string& surface_id);

private:
    void save_enrolled_surfaces_to_store();

    std::mutex mutex_;
    Blackboard* blackboard_{nullptr};
    std::atomic<uint64_t> next_client_id_{1};
    std::unordered_map<std::string, ContextRecord> contexts_;
    std::unordered_map<std::string, std::unordered_map<uint64_t, std::shared_ptr<SseClientSession>>> subscribers_;
    std::unordered_map<std::string, PairingSession> pairing_sessions_;
    std::unordered_map<std::string, EnrolledSurface> enrolled_surfaces_;
};

/**
 * @brief Agentic Blackboard High-Performance API Server
 * Exposes REST and BSON interfaces for external agents and MCP bridges.
 */
class ApiServer {
public:
    static ApiServer& instance() {
        static ApiServer inst;
        return inst;
    }

    /**
     * @brief Start the API server on the specified port and host.
     */
    void start(Blackboard* blackboard, int port = 8081, const std::string& host = "0.0.0.0");

    /**
     * @brief Stop the API server.
     */
    void stop();

    ContextBroker& context_broker() { return context_broker_; }
    void broadcast_context_event(const std::string& context_id, const std::string& event_name, const std::string& json_data) {
        context_broker_.broadcast(context_id, event_name, json_data);
    }

private:
    ApiServer() : running_(false), blackboard_(nullptr), port_(8081), host_("0.0.0.0") {}
    ~ApiServer();

    void listen_loop();
    bool authenticate_request(const httplib::Request& req, httplib::Response& res,
                              uint32_t& principal_id, std::string& authenticated_user,
                              std::string& authenticated_role,
                              nlohmann::json* out_token_meta = nullptr);

    std::atomic<bool> running_;
    std::thread thread_;
    Blackboard* blackboard_;
    int port_;
    std::string host_;
    ContextBroker context_broker_;
};

} // namespace agentic_blackboard

namespace ab = agentic_blackboard;

#pragma once

#include <zmq.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "schema.hpp"

namespace asos {
    class Blackboard;

/**
 * @brief Manages distributed node state and synchronization.
 */
class Orchestrator {
public:
    enum class State {
        CONNECTED,
        DEGRADED,
        ISOLATED
    };

    static Orchestrator& instance() {
        static Orchestrator inst;
        return inst;
    }

    void start(Blackboard* bb, const std::string& location_id, int pub_port = 8090, int sub_port = 8090);
    void stop();

    /**
     * @brief Broadcast a Knowledge Atom to all peers.
     */
    void broadcast_atom(const CpbEntry& entry);

    /**
     * @brief Dispatch a command to the Nucleus Governor via ZMQ.
     */
    void dispatch_nucleus_command(const std::string& json_payload);

    State current_state() const { return state_; }

    struct PeerInfo {
        NodeHeartbeat hb;
        int64_t last_seen;
    };
    std::unordered_map<std::string, PeerInfo> get_peers() {
        std::lock_guard<std::mutex> lock(nodes_mutex_);
        return remote_nodes_;
    }

private:

    Orchestrator() : running_(false), state_(State::CONNECTED) {}
    ~Orchestrator();

    void heartbeat_loop();
    void listen_loop();

    std::string location_id_;
    std::atomic<bool> running_;
    std::atomic<State> state_;
    
    std::thread heartbeat_thread_;
    std::thread listen_thread_;

    zmq::context_t ctx_;
    std::mutex nodes_mutex_;
    std::unordered_map<std::string, PeerInfo> remote_nodes_;

    Blackboard* blackboard_ = nullptr; // For merging incoming atoms

};



} // namespace asos

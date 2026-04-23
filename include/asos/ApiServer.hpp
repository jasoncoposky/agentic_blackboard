#pragma once

#include "asos/Blackboard.hpp"
#include <string>
#include <memory>
#include <atomic>
#include <thread>

namespace asos {

/**
 * @brief ASOS High-Performance API Server
 * Exposes REST and BSON interfaces for external agents and MCP bridges.
 */
class ApiServer {
public:
    static ApiServer& instance() {
        static ApiServer inst;
        return inst;
    }

    /**
     * @brief Start the API server on the specified port.
     */
    void start(Blackboard* blackboard, int port = 8081);

    /**
     * @brief Stop the API server.
     */
    void stop();

private:
    ApiServer() : running_(false), blackboard_(nullptr), port_(8081) {}
    ~ApiServer();

    void listen_loop();

    std::atomic<bool> running_;
    std::thread thread_;
    Blackboard* blackboard_;
    int port_;
};

} // namespace asos

#pragma once

#include "Blackboard.hpp"
#include <thread>
#include <atomic>

namespace asos {

/**
 * @brief Singleton analyzer that creates "Synapses" (Edges) between CPB atoms.
 */
class Librarian {
public:
    static Librarian& instance() {
        static Librarian inst;
        return inst;
    }

    /**
     * @brief Start the background analysis thread.
     */
    void start(Blackboard* blackboard);
    void stop();

    /**
     * @brief Force a manual analysis of the current graph.
     */
    void perform_analysis();
    void audit_orphans();


private:
    Librarian() : running_(false), blackboard_(nullptr) {}
    ~Librarian();

    void analysis_loop();
    
    std::atomic<bool> running_;
    std::thread thread_;
    Blackboard* blackboard_;
};

} // namespace asos

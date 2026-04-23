#pragma once

#include "asos/Blackboard.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace asos {

/**
 * @brief Singleton for SRE Metrics and Swarm Governance.
 */
class Monitor {
public:
    static Monitor& instance() {
        static Monitor inst;
        return inst;
    }

    void start(Blackboard* blackboard);
    void stop();
    void perform_cycle();


    /**
     * @brief Instrumentation: Report a knowledge atom commitment.
     */
    void report_atom_commit();

    /**
     * @brief Instrumentation: Report a semantic merge event.
     * @param conflict True if the merge resulted in a displacement (sidecar creation).
     */
    void report_merge_event(bool conflict);

private:
    Monitor() : running_(false), blackboard_(nullptr), commit_count_(0), total_merges_(0), conflict_merges_(0) {}
    ~Monitor();

    void monitor_loop();
    void ensure_anchors();

    std::atomic<bool> running_;
    std::thread thread_;
    Blackboard* blackboard_;

    // Metrics Counters
    std::atomic<int64_t> commit_count_;
    std::atomic<int64_t> total_merges_;
    std::atomic<int64_t> conflict_merges_;

    // Historical window for velocity calculation
    struct TimePoint {
        std::chrono::steady_clock::time_point ts;
        int64_t count;
    };
    std::vector<TimePoint> history_;
    std::mutex history_mutex_;
};

} // namespace asos

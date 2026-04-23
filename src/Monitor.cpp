#include "asos/Monitor.hpp"
#include "asos/Orchestrator.hpp"
#include <iostream>
#include <chrono>
#include <numeric>

namespace asos {

Monitor::~Monitor() {
    stop();
}

void Monitor::start(Blackboard* blackboard) {
    blackboard_ = blackboard;
    ensure_anchors();
    running_ = true;
    thread_ = std::thread(&Monitor::monitor_loop, this);
}

void Monitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Monitor::report_atom_commit() {
    commit_count_++;
}

void Monitor::report_merge_event(bool conflict) {
    total_merges_++;
    if (conflict) conflict_merges_++;
}

void Monitor::monitor_loop() {
    std::cout << "[Monitor] SRE Metrics Engine started." << std::endl;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(2)); // Faster cycle for v0.2
        
        if (!blackboard_) continue;
        perform_cycle();
    }
}

void Monitor::perform_cycle() {
    SwarmHealthSummary summary;
    
    // 1. Calculate Knowledge Velocity (Atoms per minute)
    int64_t current_commits = commit_count_.exchange(0);
    summary.metrics.knowledge_velocity = static_cast<double>(current_commits) * 30.0; // 30 * 2s = 1m

    // 2. Calculate Toil Ratio (Conflicts / Total Merges)
    int64_t m_total = total_merges_.load();
    int64_t m_conflicts = conflict_merges_.load();
    if (m_total > 0) {
        summary.metrics.toil_ratio = static_cast<double>(m_conflicts) / static_cast<double>(m_total);
    } else {
        summary.metrics.toil_ratio = 0.0;
    }

    // 3. Aggregate Sync Latency and Cluster Status
    auto peers = Orchestrator::instance().get_peers();
    int64_t total_lat = 0;
    int peer_count = 0;
    
    summary.cluster_status["local"] = (Orchestrator::instance().current_state() == Orchestrator::State::ISOLATED) ? "ISOLATED" : "CONNECTED";

    for (const auto& [id, info] : peers) {
        summary.cluster_status[id] = info.hb.connectivity_state;
        auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        int64_t lat_ms = std::abs(now_ns - info.hb.sync_epoch) / 1000000;
        total_lat += lat_ms;
        peer_count++;
    }

    summary.metrics.sync_latency_ms = (peer_count > 0) ? (total_lat / peer_count) : 0;

    // 4. Persist Swarm Health Summary
    lite3cpp::Buffer buf;
    summary.serialize(buf);
    
    std::cout << "[Monitor] Swarm Health Snapshot: Velocity=" << summary.metrics.knowledge_velocity 
              << " Toil=" << summary.metrics.toil_ratio 
              << " Latency=" << summary.metrics.sync_latency_ms << "ms" << std::endl;

    blackboard_->get_engine()->put_node("governance:swarm_health", std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
    blackboard_->get_engine()->add_edge("governance:swarm_health", rel::CREATED_BY, 1.0, "identity:governor");
    blackboard_->get_engine()->add_edge("governance:swarm_health", rel::BELONGS_TO, 1.0, "project:governance");
}

void Monitor::ensure_anchors() {
    if (!blackboard_) return;
    auto* engine = blackboard_->get_engine();

    // 1. Ensure Global Governance Project
    if (!engine->get_node("project:governance")) {
        ProjectNode pn;
        pn.project_id = "project:governance";
        pn.description = "Global ASOS Governance & SRE Substrate";
        pn.lifecycle_status = "ACTIVE";
        
        lite3cpp::Buffer buf;
        pn.serialize(buf);
        engine->put_node(pn.project_id, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
        std::cout << "[Monitor] Created System Project: " << pn.project_id << std::endl;
    }

    // 2. Ensure System Governor Identity
    if (!engine->get_node("identity:governor")) {
        IdentityNode in;
        in.id = "identity:governor";
        in.display_name = "System Governor";
        in.role = "AUTONOMOUS_SRE";
        
        lite3cpp::Buffer buf;
        in.serialize(buf);
        engine->put_node(in.id, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
        std::cout << "[Monitor] Created System Identity: " << in.id << std::endl;
    }
}


} // namespace asos

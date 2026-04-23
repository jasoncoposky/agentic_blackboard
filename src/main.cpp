#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include "asos/Blackboard.hpp"
#include "asos/Librarian.hpp"
#include "asos/Orchestrator.hpp"
#include "asos/Validator.hpp"
#include "asos/Monitor.hpp"
#include "asos/ApiServer.hpp"

#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"

std::atomic<bool> g_running(true);

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[ASOS] Shutdown signal received (" << signal << ")..." << std::endl;
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    std::string db_path = "asos_db";

    uint32_t node_id = 1;

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (isdigit(arg[0])) node_id = std::stoi(arg);
            else if (arg.find("--db=") == 0) db_path = arg.substr(5);
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        std::cout << "--- ASOS Blackboard Engine v0.4 (Substrate Complete) ---" << std::endl;
        
        // 1. Initialize Substrate (Storage)
        asos::Blackboard bb(db_path, node_id);

        // -- SEEDING LOGIC --
        bool should_exit = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--seed") {
                std::cout << "[ASOS] Seeding Substrate with Identity and Project Anchors..." << std::endl;
                
                // Agents
                asos::IdentityNode agent1 = {"Nexus_Agent_7", "Nexus Agent 7", "Lead Architect", "pub-key-n7"};
                asos::IdentityNode agent2 = {"auditor-prime", "Auditor Prime", "SWEBOK Compliance", "pub-key-ap"};
                if (bb.commit_identity_node(agent1)) std::cout << "[SEED] Committed Identity: Nexus_Agent_7" << std::endl;
                if (bb.commit_identity_node(agent2)) std::cout << "[SEED] Committed Identity: auditor-prime" << std::endl;

                // Projects
                asos::ProjectNode proj1 = {"ALPHA_SWARM", "Distributed Resilience Initiative", "ACTIVE"};
                asos::ProjectNode proj2 = {"shakedown-final", "Final Integration Verification", "ACTIVE"};
                if (bb.commit_project_node(proj1)) std::cout << "[SEED] Committed Project: ALPHA_SWARM" << std::endl;
                if (bb.commit_project_node(proj2)) std::cout << "[SEED] Committed Project: shakedown-final" << std::endl;

                // Atoms
                asos::CpbEntry atom1;
                atom1.header.uuid = "atom-raft-01";
                atom1.header.origin.agent_id = "Nexus_Agent_7";
                atom1.header.origin.project_id = "ALPHA_SWARM";
                atom1.payload.statement = "Raft consensus requires a majority of nodes for stability.";
                atom1.taxonomy.knowledge_area = asos::KnowledgeArea::COMPUTING_FOUNDATIONS;
                bb.commit_cpb_entry(atom1);

                asos::CpbEntry atom2;
                atom2.header.uuid = "atom-auditor-01";
                atom2.header.origin.agent_id = "auditor-prime";
                atom2.header.origin.project_id = "shakedown-final";
                atom2.payload.statement = "SWEBOK audit of distributed swarm health passed.";
                atom2.taxonomy.knowledge_area = asos::KnowledgeArea::ENGINEERING_MANAGEMENT;
                bb.commit_cpb_entry(atom2);

                std::cout << "[ASOS] Seed Complete. Substrate Prepared with 2 Agents, 2 Projects, and 2 Atoms." << std::endl;
                should_exit = true;


            }
        }

        if (should_exit) return 0;

        
        // 2. Start Distributed Connectivity
        std::string loc = (node_id == 1) ? "Apex_NC" : "Pittsburgh_PA";
        asos::Orchestrator::instance().start(&bb, loc, 8090, 8090);
        
        // 3. Start Observability & API
        asos::Monitor::instance().start(&bb);
        asos::ApiServer::instance().start(&bb, 8085);


        // 4. Start Intelligence & Governance

        asos::Librarian::instance().start(&bb);
        // Validator is reactive, no thread needed yet.

        std::cout << "[ASOS] Swarm Substrate Active. Governance Governor running." << std::endl;

        auto* store = bb.get_engine()->get_store();

        // 5. Governor Loop (System Health Monitoring)
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            // Periodically check Swarm Health
            auto raw = store->get(std::string(l3kvg::KeyBuilder::node_key("governance:swarm_health")));
            if (raw.size() > 0) {
                auto summary = asos::SwarmHealthSummary::deserialize(raw);
                
                std::cout << "[Governor] Health: Velocity=" << summary.metrics.knowledge_velocity 
                          << " | Toil=" << summary.metrics.toil_ratio 
                          << " | Latency=" << summary.metrics.sync_latency_ms << "ms" << std::endl;

                // Architectural Alert logic
                if (summary.metrics.toil_ratio > 0.7) {
                    std::cout << "[ALERT] HIGH TOIL DETECTED: Friction in distributed reconciliation. Intervention suggested." << std::endl;
                }
            } else {
                std::cout << "[Governor] Waiting for first health snapshot..." << std::endl;
            }
        }

        // 6. Graceful Atomic Shutdown Sequence
        std::cout << "[ASOS] Initiating graceful shutdown..." << std::endl;
        
        asos::Librarian::instance().stop();    // Stop intelligence first
        asos::Monitor::instance().stop();      // Stop metrics second
        asos::Orchestrator::instance().stop(); // Stop network third
        
        std::cout << "[ASOS] Clean shutdown complete. Substrate de-commissioned." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[CRITICAL] ASOS Daemon Crash: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

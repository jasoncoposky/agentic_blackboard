#include <iostream>
#include <fstream>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <thread>
#include <agentic_blackboard/Blackboard.hpp>
#include <agentic_blackboard/Librarian.hpp>
#include <agentic_blackboard/Orchestrator.hpp>
#include <agentic_blackboard/Validator.hpp>
#include <agentic_blackboard/Monitor.hpp>
#include <agentic_blackboard/ApiServer.hpp>

#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"
#include <cstdlib>
#include <random>
#include <cstring>
#if defined(__linux__)
#include <dlfcn.h>
#endif

#if defined(__linux__)
extern "C" {
long int __isoc23_strtol(const char *nptr, char **endptr, int base) {
    static auto real_fn = (long int (*)(const char*, char**, int))dlsym(RTLD_DEFAULT, "strtol");
    if (real_fn) return real_fn(nptr, endptr, base);
    return 0;
}
long long int __isoc23_strtoll(const char *nptr, char **endptr, int base) {
    static auto real_fn = (long long int (*)(const char*, char**, int))dlsym(RTLD_DEFAULT, "strtoll");
    if (real_fn) return real_fn(nptr, endptr, base);
    return 0;
}
unsigned long int __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    static auto real_fn = (unsigned long int (*)(const char*, char**, int))dlsym(RTLD_DEFAULT, "strtoul");
    if (real_fn) return real_fn(nptr, endptr, base);
    return 0;
}
unsigned long long int __isoc23_strtoull(const char *nptr, char **endptr, int base) {
    static auto real_fn = (unsigned long long int (*)(const char*, char**, int))dlsym(RTLD_DEFAULT, "strtoull");
    if (real_fn) return real_fn(nptr, endptr, base);
    return 0;
}

uint32_t arc4random(void) {
    static thread_local std::mt19937 gen(std::chrono::high_resolution_clock::now().time_since_epoch().count() ^ (uintptr_t)&gen);
    return gen();
}
void arc4random_buf(void *buf, size_t nbytes) {
    uint8_t *p = static_cast<uint8_t*>(buf);
    while (nbytes >= 4) {
        uint32_t r = arc4random();
        std::memcpy(p, &r, 4);
        p += 4;
        nbytes -= 4;
    }
    if (nbytes > 0) {
        uint32_t r = arc4random();
        std::memcpy(p, &r, nbytes);
    }
}
uint32_t arc4random_uniform(uint32_t upper_bound) {
    if (upper_bound <= 1) return 0;
    return arc4random() % upper_bound;
}
}
#endif

std::atomic<bool> g_running(true);

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\n[AgenticBlackboard] Shutdown signal received (" << signal << ")..." << std::endl;
        g_running = false;
    }
}

int main(int argc, char* argv[]) {
    std::string db_path = "ab_db";
    uint32_t node_id = 1;
    std::string auth_mode = "trusted_network";

    std::string admin_token;
    int port = 8085;
    std::string host = "0.0.0.0";

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (isdigit(arg[0])) node_id = std::stoi(arg);
            else if (arg.find("--db=") == 0) db_path = arg.substr(5);
            else if (arg.find("--data-dir=") == 0) db_path = arg.substr(11) + "/ab_db";
            else if (arg.find("--auth-mode=") == 0) auth_mode = arg.substr(12);
            else if (arg.find("--admin-token=") == 0) admin_token = arg.substr(14);
            else if (arg.find("--port=") == 0) port = std::stoi(arg.substr(7));
            else if (arg.find("--host=") == 0) host = arg.substr(7);
            else if (arg.find("--config=") == 0) {
                std::string conf_file = arg.substr(9);
                std::ifstream cf(conf_file);
                if (!cf.is_open()) {
                    std::cerr << "[CRITICAL] Unable to open configuration file: " << conf_file << std::endl;
                    return 1;
                }
                std::string line;
                while (std::getline(cf, line)) {
                    size_t start = line.find_first_not_of(" \t\r\n");
                    if (start == std::string::npos || line[start] == '#' || line[start] == ';') continue;
                    size_t end = line.find_last_not_of(" \t\r\n");
                    std::string trimmed = line.substr(start, end - start + 1);
                    if (trimmed.front() == '[' && trimmed.back() == ']') continue;
                    auto eq = trimmed.find('=');
                    if (eq != std::string::npos) {
                        std::string k = trimmed.substr(0, eq);
                        std::string v = trimmed.substr(eq + 1);
                        size_t ks = k.find_first_not_of(" \t");
                        size_t ke = k.find_last_not_of(" \t");
                        if (ks != std::string::npos) k = k.substr(ks, ke - ks + 1);
                        size_t vs = v.find_first_not_of(" \t");
                        size_t ve = v.find_last_not_of(" \t");
                        if (vs != std::string::npos) v = v.substr(vs, ve - vs + 1);

                        if (k == "port") port = std::stoi(v);
                        else if (k == "host") host = v;
                        else if (k == "mode" || k == "auth_mode") auth_mode = v;
                        else if (k == "data_dir") {
                            db_path = v + "/ab_db";
                        }
                    }
                }
            }
        }
    }

    // Auto-discover admin token if not passed directly
    if (admin_token.empty()) {
        std::string parent_dir = "ab_db";
        auto pos = db_path.find_last_of("/\\");
        if (pos != std::string::npos) {
            parent_dir = db_path.substr(0, pos);
        }
        std::vector<std::string> token_candidates = {
            parent_dir + "/admin.token",
            "/var/lib/agentic-blackboard/admin.token",
            "/etc/agentic-blackboard/admin.token"
        };
        for (const auto& tc : token_candidates) {
            std::ifstream tf(tc);
            if (tf.is_open()) {
                std::string tok;
                if (tf >> tok && !tok.empty()) {
                    admin_token = tok;
                    break;
                }
            }
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        std::cout << "--- Agentic Blackboard Engine v0.4 (Substrate Complete) ---" << std::endl;
        
        // 1. Initialize Substrate (Storage)
        agentic_blackboard::Blackboard bb(db_path, node_id);
        bb.set_auth_mode(auth_mode);
        if (!admin_token.empty()) {
            bb.register_token(admin_token, "admin", "admin");
        }

        // -- SEEDING LOGIC --
        bool should_exit = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--seed") {
                std::cout << "[AgenticBlackboard] Seeding Substrate with Identity and Project Anchors..." << std::endl;
                
                // Seed auth tokens for admin and curator
                bb.register_token("ab_adm_0123456789abcdef0123456789abcdef", "admin", "admin");
                bb.register_token("ab_usr_fedcba9876543210fedcba9876543210", "alice", "curator");
                
                // Agents
                agentic_blackboard::IdentityNode agent1 = {"Nexus_Agent_7", "Nexus Agent 7", "Lead Architect", "pub-key-n7"};
                agentic_blackboard::IdentityNode agent2 = {"auditor-prime", "Auditor Prime", "SWEBOK Compliance", "pub-key-ap"};
                if (bb.commit_identity_node(agent1)) std::cout << "[SEED] Committed Identity: Nexus_Agent_7" << std::endl;
                if (bb.commit_identity_node(agent2)) std::cout << "[SEED] Committed Identity: auditor-prime" << std::endl;

                // Projects
                agentic_blackboard::ProjectNode proj1 = {"ALPHA_SWARM", "Distributed Resilience Initiative", "ACTIVE"};
                agentic_blackboard::ProjectNode proj2 = {"shakedown-final", "Final Integration Verification", "ACTIVE"};
                if (bb.commit_project_node(proj1)) std::cout << "[SEED] Committed Project: ALPHA_SWARM" << std::endl;
                if (bb.commit_project_node(proj2)) std::cout << "[SEED] Committed Project: shakedown-final" << std::endl;

                // Atoms
                agentic_blackboard::CpbEntry atom1;
                atom1.header.uuid = "atom-raft-01";
                atom1.header.origin.agent_id = "Nexus_Agent_7";
                atom1.header.origin.project_id = "ALPHA_SWARM";
                atom1.payload.statement = "Raft consensus requires a majority of nodes for stability.";
                atom1.taxonomy.knowledge_area = agentic_blackboard::KnowledgeArea::COMPUTING_FOUNDATIONS;
                bb.commit_cpb_entry(atom1);

                agentic_blackboard::CpbEntry atom2;
                atom2.header.uuid = "atom-auditor-01";
                atom2.header.origin.agent_id = "auditor-prime";
                atom2.header.origin.project_id = "shakedown-final";
                atom2.payload.statement = "SWEBOK audit of distributed swarm health passed.";
                atom2.taxonomy.knowledge_area = agentic_blackboard::KnowledgeArea::ENGINEERING_MANAGEMENT;
                bb.commit_cpb_entry(atom2);

                std::cout << "[AgenticBlackboard] Seed Complete. Substrate Prepared with 2 Agents, 2 Projects, and 2 Atoms." << std::endl;
                should_exit = true;


            }
        }

        if (should_exit) return 0;

        
        // 2. Start Distributed Connectivity
        std::string loc = (node_id == 1) ? "Apex_NC" : "Pittsburgh_PA";
        agentic_blackboard::Orchestrator::instance().start(&bb, loc, 8090, 8090);
        
        // 3. Start Observability & API
        agentic_blackboard::Monitor::instance().start(&bb);
        agentic_blackboard::ApiServer::instance().start(&bb, port, host);


        // 4. Start Intelligence & Governance

        agentic_blackboard::Librarian::instance().start(&bb);
        // Validator is reactive, no thread needed yet.

        std::cout << "[AgenticBlackboard] Swarm Substrate Active. Governance Governor running." << std::endl;

        auto* store = bb.get_engine()->get_store();

        // 5. Governor Loop (System Health Monitoring)
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            // Periodically check Swarm Health
            auto raw = store->get(std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("governance:swarm_health"))));
            if (raw.size() > 0) {
                auto summary = agentic_blackboard::SwarmHealthSummary::deserialize(raw);
                
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
        std::cout << "[AgenticBlackboard] Initiating graceful shutdown..." << std::endl;
        
        agentic_blackboard::ApiServer::instance().stop();    // Stop API server and disconnect SSE clients first
        agentic_blackboard::Librarian::instance().stop();    // Stop intelligence second
        agentic_blackboard::Monitor::instance().stop();      // Stop metrics third
        agentic_blackboard::Orchestrator::instance().stop(); // Stop network fourth
        
        std::cout << "[AgenticBlackboard] Clean shutdown complete. Substrate de-commissioned." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[CRITICAL] Agentic Blackboard Daemon Crash: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

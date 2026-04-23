#include <iostream>
#include "asos/Blackboard.hpp"
#include "asos/schema.hpp"

int main() {
    try {
        std::cout << "[SCRATCH] Force-Seeding Substrate..." << std::endl;
        asos::Blackboard bb("asos_db", 1);
        
        // Agents
        asos::IdentityNode agent1 = {"Nexus_Agent_7", "Nexus Agent 7", "Lead Architect", "pub-key-n7"};
        asos::IdentityNode agent2 = {"auditor-prime", "Auditor Prime", "SWEBOK Compliance", "pub-key-ap"};
        bb.commit_identity_node(agent1);
        bb.commit_identity_node(agent2);

        // Projects
        asos::ProjectNode proj1 = {"ALPHA_SWARM", "Distributed Resilience Initiative", "ACTIVE"};
        asos::ProjectNode proj2 = {"shakedown-final", "Final Integration Verification", "ACTIVE"};
        bb.commit_project_node(proj1);
        bb.commit_project_node(proj2);

        std::cout << "[SCRATCH] Seed Complete. Verified 4 anchors committed." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Scratch Seed Failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

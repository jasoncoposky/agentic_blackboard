#include <iostream>
#include <thread>
#include <chrono>
#include "asos/Blackboard.hpp"
#include "asos/schema.hpp"

int main() {
    try {
        std::cout << "[DEBUG] Re-seeding with forced wait..." << std::endl;
        asos::Blackboard bb("asos_db", 1);
        
        // Identity
        asos::IdentityNode agent = {"Nexus_Agent_7", "Nexus Agent 7", "Lead Architect", "pub-key-n7"};
        bb.commit_identity_node(agent);

        // Project
        asos::ProjectNode proj = {"ALPHA_SWARM", "Distributed Resilience Initiative", "ACTIVE"};
        bb.commit_project_node(proj);

        // Atom
        asos::CpbEntry atom;
        atom.header.uuid = "atom-raft-01";
        atom.header.origin.agent_id = "Nexus_Agent_7";
        atom.header.origin.project_id = "ALPHA_SWARM";
        atom.payload.statement = "Raft consensus requires a majority of nodes for stability.";
        atom.taxonomy.knowledge_area = asos::KnowledgeArea::COMPUTING_FOUNDATIONS;
        
        if (bb.commit_cpb_entry(atom)) {
            std::cout << "Atom committed. Waiting 2 seconds for flusher..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        } else {
            std::cout << "Atom REJECTED!" << std::endl;
        }

        std::cout << "Done." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}

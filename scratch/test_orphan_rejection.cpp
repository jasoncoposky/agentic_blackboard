#include <iostream>
#include "asos/Blackboard.hpp"
#include "asos/schema.hpp"

int main() {
    try {
        std::cout << "[TEST] Running Orphan Rejection Verification..." << std::endl;
        asos::Blackboard bb("test_asos_db", 99);
        
        // 1. Valid Atom (Both Anchors)
        asos::CpbEntry valid_atom;
        valid_atom.header.uuid = "valid-1";
        valid_atom.header.origin.agent_id = "test-agent";
        valid_atom.header.origin.project_id = "test-project";
        valid_atom.payload.statement = "This should be accepted.";
        
        if (bb.commit_cpb_entry(valid_atom)) {
            std::cout << "[PASS] Valid atom accepted." << std::endl;
        } else {
            std::cout << "[FAIL] Valid atom rejected!" << std::endl;
        }

        // 2. Orphan Atom (Missing Project)
        asos::CpbEntry orphan_1;
        orphan_1.header.uuid = "orphan-1";
        orphan_1.header.origin.agent_id = "test-agent";
        orphan_1.header.origin.project_id = ""; // Missing
        
        if (!bb.commit_cpb_entry(orphan_1)) {
            std::cout << "[PASS] Orphan atom (missing project) rejected correctly." << std::endl;
        } else {
            std::cout << "[FAIL] Orphan atom (missing project) was accepted!" << std::endl;
        }

        // 3. Orphan Atom (Missing Agent)
        asos::CpbEntry orphan_2;
        orphan_2.header.uuid = "orphan-2";
        orphan_2.header.origin.agent_id = ""; // Missing
        orphan_2.header.origin.project_id = "test-project";
        
        if (!bb.commit_cpb_entry(orphan_2)) {
            std::cout << "[PASS] Orphan atom (missing agent) rejected correctly." << std::endl;
        } else {
            std::cout << "[FAIL] Orphan atom (missing agent) was accepted!" << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Test failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

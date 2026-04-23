#include "asos/Blackboard.hpp"
#include "asos/Orchestrator.hpp"
#include "asos/Librarian.hpp"
#include "asos/Validator.hpp"
#include "asos/DeltaEngine.hpp"
#include "asos/Monitor.hpp"
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <cassert>

using namespace asos;

void test_identity_integrity() {
    std::cout << "[Test] Starting Identity Integrity Verification..." << std::endl;
    Blackboard bb("test_identity_db", 6);
    auto* store = bb.get_engine()->get_store();

    // 1. Commit an atom with a NEW agent and project
    CpbEntry e;
    e.header.uuid = "identity-atom-1";
    e.header.origin.agent_id = "agent-x";
    e.header.origin.project_id = "project-y";
    e.taxonomy.applicability = 50;
    e.payload.statement = "Identity anchor test node";
    
    bb.commit_cpb_entry(e);
    store->wait_all_shards();

    // 2. Verify Identity Node (Anchor) was auto-created
    auto id_node = bb.get_engine()->get_node("agent-x");
    if (!id_node) {
        std::cerr << "[Test] FAILED: Identity node 'agent-x' should have been auto-created" << std::endl;
        exit(1);
    }
    std::cout << "[Test] Auto-anchored Identity node verified" << std::endl;

    // 3. Verify Project Node (Anchor) was auto-created
    auto proj_node = bb.get_engine()->get_node("project-y");
    if (!proj_node) {
        std::cerr << "[Test] FAILED: Project node 'project-y' should have been auto-created" << std::endl;
        exit(1);
    }
    std::cout << "[Test] Auto-anchored Project node verified" << std::endl;

    // 4. Verify Edges (Semantic Links)
    auto atom_node = bb.get_engine()->get_node("identity-atom-1");
    bool has_author = false;
    bool has_project = false;

    auto author_edges = atom_node->get_edges(rel::CREATED_BY);
    for (auto& edge : author_edges) {
        if (edge->get_dst() == "agent-x") has_author = true;
    }

    auto project_edges = atom_node->get_edges(rel::BELONGS_TO);
    for (auto& edge : project_edges) {
        if (edge->get_dst() == "project-y") has_project = true;
    }

    if (!has_author || !has_project) {
        std::cerr << "[Test] FAILED: Semantic links (edges) missing for identity-atom-1" << std::endl;
        exit(1);
    }
    std::cout << "[Test] Identity and Project edges verified" << std::endl;

    // 5. Run Librarian Orphan Audit
    Librarian::instance().start(&bb);
    Librarian::instance().audit_orphans();
    Librarian::instance().stop();

    std::cout << "[Test] Identity Integrity Verification PASSED" << std::endl;
}

void test_semantic_merge() {
    std::cout << "[Test] Starting Semantic Merge Verification..." << std::endl;
    Blackboard bb("test_merge_db", 1);
    auto* store = bb.get_engine()->get_store();
    
    CpbEntry e1;
    e1.header.uuid = "atom-001";
    e1.header.timestamp = 1000;
    e1.header.origin.agent_id = "agent-1";
    e1.header.origin.project_id = "proj-1";
    e1.taxonomy.applicability = 50;
    e1.taxonomy.knowledge_area = KnowledgeArea::DESIGN;
    e1.payload.statement = "Data version 1";
    
    bb.commit_cpb_entry(e1);
    store->wait_all_shards();

    CpbEntry e2;
    e2.header.uuid = "atom-001";
    e2.header.timestamp = 1100;
    e2.header.origin.agent_id = "agent-1";
    e2.header.origin.project_id = "proj-1";
    e2.taxonomy.applicability = 80;
    e2.taxonomy.knowledge_area = KnowledgeArea::DESIGN;
    e2.payload.statement = "Data version 2 (Winner)";
    
    bool won = bb.semantic_merge(e2);
    store->wait_all_shards();
    if (!won) {
        std::cerr << "[Test] FAILED: e2 should have won (app 80 > 50)" << std::endl;
        exit(1);
    }

    CpbEntry e3;
    e3.header.uuid = "atom-001";
    e3.header.timestamp = 1200;
    e3.header.origin.agent_id = "agent-1";
    e3.header.origin.project_id = "proj-1";
    e3.taxonomy.applicability = 30;
    e3.taxonomy.knowledge_area = KnowledgeArea::DESIGN;
    e3.payload.statement = "Data version 3 (Loser)";
    
    won = bb.semantic_merge(e3);
    store->wait_all_shards();
    if (won) {
        std::cerr << "[Test] FAILED: e3 should have lost (app 30 < 80)" << std::endl;
        exit(1);
    }

    auto key = std::string(l3kvg::KeyBuilder::node_key("atom-001"));
    auto raw = store->get(key);
    CpbEntry current = CpbEntry::deserialize(raw);
    if (current.taxonomy.applicability != 80) {
        std::cerr << "[Test] FAILED: Final applicability is " << current.taxonomy.applicability << " but expected 80" << std::endl;
        exit(1);
    }

    std::cout << "[Test] Semantic Merge PASSED" << std::endl;
}

void test_sre_metrics() {
    std::cout << "[Test] Starting SRE Metrics Verification..." << std::endl;
    Blackboard bb("test_metrics_db", 5);
    auto* store = bb.get_engine()->get_store();

    for (int i = 0; i < 5; ++i) {
        CpbEntry e;
        e.header.uuid = "metric-atom-" + std::to_string(i);
        e.header.origin.agent_id = "agent-metrics";
        e.header.origin.project_id = "proj-metrics";
        e.taxonomy.applicability = 50;
        bb.commit_cpb_entry(e);
    }

    for (int i = 0; i < 5; ++i) {
        CpbEntry incoming;
        incoming.header.uuid = "metric-atom-" + std::to_string(i);
        incoming.header.origin.agent_id = "agent-metrics";
        incoming.header.origin.project_id = "proj-metrics";
        incoming.header.timestamp = 2000;
        incoming.taxonomy.applicability = (i < 2) ? 10 : 90;
        bb.semantic_merge(incoming);
    }
    store->wait_all_shards();

    Monitor::instance().start(&bb);
    Monitor::instance().perform_cycle();
    store->wait_all_shards();

    lite3cpp::Buffer raw = store->get(std::string(l3kvg::KeyBuilder::node_key("governance:swarm_health")));
    if (raw.size() == 0) {
        std::cerr << "[Test] FAILED: Could not find swarm health node" << std::endl;
        exit(1);
    }

    SwarmHealthSummary summary = SwarmHealthSummary::deserialize(raw);
    if (summary.metrics.knowledge_velocity <= 0) {
        std::cerr << "[Test] FAILED: Knowledge velocity should be > 0" << std::endl;
        exit(1);
    }
    Monitor::instance().stop();
    std::cout << "[Test] SRE Metrics Verification PASSED" << std::endl;
}

void test_delta_sync() {
    std::cout << "[Test] Starting Binary Delta Sync Verification..." << std::endl;
    Blackboard bb("test_delta_db", 4);
    auto* store = bb.get_engine()->get_store();

    CpbEntry v1;
    v1.header.uuid = "delta-atom";
    v1.header.origin.agent_id = "agent-delta";
    v1.header.origin.project_id = "proj-delta";
    v1.header.timestamp = 100;
    v1.payload.statement = "Initial content string for delta test.";
    bb.commit_cpb_entry(v1);
    store->wait_all_shards();

    lite3cpp::Buffer b1_raw = store->get(std::string(l3kvg::KeyBuilder::node_key("delta-atom")));

    CpbEntry v2 = v1;
    v2.header.timestamp = 200;
    v2.payload.statement = "Initial content string for delta test. (MODIFIED)";
    
    lite3cpp::Buffer b2_raw;
    v2.serialize(b2_raw);

    L3DeltaPatch patch = DeltaEngine::create_xor_patch(b1_raw, b2_raw);
    patch.header.base_uuid = "delta-atom";
    patch.header.base_timestamp = 100;

    bool applied = bb.apply_delta_patch(patch);
    store->wait_all_shards();

    if (!applied) {
        std::cerr << "[Test] FAILED: Delta patch application failed" << std::endl;
        exit(1);
    }

    lite3cpp::Buffer reconstructed_raw = store->get(std::string(l3kvg::KeyBuilder::node_key("delta-atom")));
    if (reconstructed_raw.size() != b2_raw.size()) {
        std::cerr << "[Test] FAILED: Size mismatch" << std::endl;
        exit(1);
    }

    std::cout << "[Test] Binary Delta Sync Verification PASSED" << std::endl;
}

void test_knowledge_lifecycle() {
    std::cout << "[Test] Starting Knowledge Lifecycle Verification..." << std::endl;
    Blackboard bb("test_lifecycle_db", 3);
    auto* store = bb.get_engine()->get_store();
    Validator validator(&bb);

    CpbEntry atom;
    atom.header.uuid = "principle-base";
    atom.header.origin.agent_id = "agent-lifecycle";
    atom.header.origin.project_id = "proj-lifecycle";
    atom.taxonomy.applicability = 10;
    atom.taxonomy.knowledge_area = KnowledgeArea::CONFIG_MANAGEMENT;
    atom.payload.statement = "Baseline Principle Draft";
    bb.commit_cpb_entry(atom);
    store->wait_all_shards();

    validator.verify_atom("principle-base", "auditor-007");
    validator.validate_atom("principle-base", "auditor-007");
    store->wait_all_shards();

    bool promoted = validator.try_promote("principle-base");
    store->wait_all_shards();
    if (!promoted) {
        std::cerr << "[Test] FAILED: Atom should have been promoted to Principle" << std::endl;
        exit(1);
    }

    std::cout << "[Test] Knowledge Lifecycle PASSED" << std::endl;
}

void test_librarian_synapses() {
    std::cout << "[Test] Starting Librarian Synapse Verification..." << std::endl;
    Blackboard bb("test_librarian_db", 2);
    auto* store = bb.get_engine()->get_store();

    CpbEntry a1;
    a1.header.uuid = "atom-101";
    a1.header.origin.agent_id = "agent-analogy";
    a1.header.origin.project_id = "proj-analogy";
    a1.taxonomy.knowledge_area = KnowledgeArea::DESIGN;
    a1.taxonomy.tags = {"design", "pattern", "c++"};
    a1.payload.statement = "Visitor pattern in C++";
    bb.commit_cpb_entry(a1);

    CpbEntry a2;
    a2.header.uuid = "atom-102";
    a2.header.origin.agent_id = "agent-analogy";
    a2.header.origin.project_id = "proj-analogy";
    a2.taxonomy.knowledge_area = KnowledgeArea::DESIGN;
    a2.taxonomy.tags = {"design", "performance", "pattern"};
    a2.payload.statement = "Observer pattern for perf";
    bb.commit_cpb_entry(a2);

    store->wait_all_shards();

    Librarian::instance().start(&bb);
    Librarian::instance().perform_analysis();
    store->wait_all_shards();

    auto node1 = bb.get_engine()->get_node("atom-101");
    auto edges = node1->get_edges(rel::CPB_SIMILARITY);

    if (edges.empty()) {
        std::cerr << "[Test] FAILED: Could not find high-confidence CPB_SIMILARITY edge" << std::endl;
        exit(1);
    }

    Librarian::instance().stop();
    std::cout << "[Test] Librarian Synapse Verification PASSED" << std::endl;
}

void test_safe_mode() {
    std::cout << "[Test] Starting Safe-Mode Verification..." << std::endl;
    Orchestrator::instance().start("test_loc", 8092, 8093);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    
    auto state = Orchestrator::instance().current_state();
    if (state != Orchestrator::State::ISOLATED) {
        std::cerr << "[Test] FAILED: Node state expected ISOLATED" << std::endl;
        exit(1);
    }
    
    Orchestrator::instance().stop();
    std::cout << "[Test] Safe-Mode Verification PASSED" << std::endl;
}

int main() {
    try {
        test_identity_integrity();
        test_semantic_merge();
        test_sre_metrics();
        test_knowledge_lifecycle();
        test_librarian_synapses();
        test_delta_sync();
        test_safe_mode();
        std::cout << "\n[SUCCESS] All ASOS Verification Tests Passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[Test] ERROR: Catch-all exception: " << e.what() << std::endl;
        return 1;
    }
}

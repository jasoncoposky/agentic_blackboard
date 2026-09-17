#include "asos/Blackboard.hpp"
#include "asos/Orchestrator.hpp"
#include "asos/Librarian.hpp"
#include "asos/Validator.hpp"
#include "asos/DeltaEngine.hpp"
#include "asos/Monitor.hpp"
#include "asos/RdfExporter.hpp"
#include "asos/ApiServer.hpp"
#include "httplib.h"
#include <nlohmann/json.hpp>
#include "engine/store.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/Node.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <cassert>
#include <filesystem>

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
        if (edge->get_dst() == bb.get_engine()->get_resolver().parse_uuid("agent-x")) has_author = true;
    }

    auto project_edges = atom_node->get_edges(rel::BELONGS_TO);
    for (auto& edge : project_edges) {
        if (edge->get_dst() == bb.get_engine()->get_resolver().parse_uuid("project-y")) has_project = true;
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

    auto key = std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("atom-001")));
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

    lite3cpp::Buffer raw = store->get(std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("governance:swarm_health"))));
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

    lite3cpp::Buffer b1_raw = store->get(std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("delta-atom"))));

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

    lite3cpp::Buffer reconstructed_raw = store->get(std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("delta-atom"))));
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
    Blackboard bb("test_safe_db", 6);
    Orchestrator::instance().start(&bb, "test_loc", 8092, 8093);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    
    auto state = Orchestrator::instance().current_state();
    if (state != Orchestrator::State::ISOLATED) {
        std::cerr << "[Test] FAILED: Node state expected ISOLATED" << std::endl;
        exit(1);
    }
    
    Orchestrator::instance().stop();
    std::cout << "[Test] Safe-Mode Verification PASSED" << std::endl;
}

void test_life_journaling() {
    std::cout << "[Test] Starting Life Journaling Verification..." << std::endl;
    std::filesystem::remove_all("test_journal_db");
    Blackboard bb("test_journal_db", 7);
    auto* store = bb.get_engine()->get_store();

    // 1. Setup multi-tenant user credentials
    bb.register_user_credentials("jasoncoposky", "jason-key");
    uint32_t jason_uid = bb.get_user_uid("jasoncoposky");

    bb.register_user_credentials("intruder_bob", "bob-key");
    uint32_t bob_uid = bb.get_user_uid("intruder_bob");

    // 2. Create a wellness journal entry under jasoncoposky's namespace
    CpbEntry w_entry;
    w_entry.header.uuid = "jasoncoposky:journal-well-1";
    w_entry.header.timestamp = 1000;
    w_entry.header.origin.agent_id = "identity:jasoncoposky";
    w_entry.header.origin.project_id = "proj-journal";
    w_entry.taxonomy.knowledge_area = KnowledgeArea::HEALTH_WELLNESS;
    w_entry.taxonomy.applicability = 95;
    w_entry.payload.statement = "Morning Run and Mood check";
    
    Wellness w;
    w.mood_sentiment = 0.8;
    w.energy_level = 9.0;
    w.sleep_hours = 8.0;
    w.active_minutes = 45.0;
    w.step_count = 12000;
    w.activity_type = "running";
    w_entry.wellness = w;

    // Jason writes his own entry -> Expect SUCCESS
    bool jason_write_ok = bb.commit_cpb_entry(w_entry, jason_uid);
    if (!jason_write_ok) {
        std::cerr << "[Test] FAILED: Jason could not commit his own wellness entry" << std::endl;
        exit(1);
    }
    store->wait_all_shards();

    // Intruder Bob attempts to overwrite/impersonate Jason -> Expect FAILURE
    bool bob_write_ok = bb.commit_cpb_entry(w_entry, bob_uid);
    if (bob_write_ok) {
        std::cerr << "[Test] FAILED: Intruder Bob was allowed to commit on behalf of Jason" << std::endl;
        exit(1);
    }

    // 3. Create an education journal entry under jasoncoposky's namespace
    CpbEntry e_entry;
    e_entry.header.uuid = "jasoncoposky:journal-edu-1";
    e_entry.header.timestamp = 2000;
    e_entry.header.origin.agent_id = "identity:jasoncoposky";
    e_entry.header.origin.project_id = "proj-journal";
    e_entry.taxonomy.knowledge_area = KnowledgeArea::EDUCATION_LEARNING;
    e_entry.taxonomy.applicability = 90;
    e_entry.payload.statement = "Abstract Algebra Lecture 1";

    Education edu;
    edu.institution_platform = "MIT OpenCourseWare";
    edu.resource_type = "lecture";
    edu.progress_percent = 25.0;
    edu.focus_duration_minutes = 60.0;
    edu.credential_uuid = "cred-algebra-101";
    e_entry.education = edu;

    bool jason_edu_write_ok = bb.commit_cpb_entry(e_entry, jason_uid);
    if (!jason_edu_write_ok) {
        std::cerr << "[Test] FAILED: Jason could not commit his own education entry" << std::endl;
        exit(1);
    }
    store->wait_all_shards();

    // 4. Query back wellness entry using Jason's UID -> Expect SUCCESS
    auto key_well = std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("jasoncoposky:journal-well-1")));
    auto buf_well = store->get(key_well, jason_uid);
    if (buf_well.size() == 0) {
        std::cerr << "[Test] FAILED: Jason could not retrieve his own wellness journal node" << std::endl;
        exit(1);
    }
    
    CpbEntry fetched_well = CpbEntry::deserialize(buf_well);
    if (!fetched_well.wellness) {
        std::cerr << "[Test] FAILED: Deserialized wellness block is missing" << std::endl;
        exit(1);
    }
    if (fetched_well.wellness->mood_sentiment != 0.8 || fetched_well.wellness->energy_level != 9.0 ||
        fetched_well.wellness->sleep_hours != 8.0 || fetched_well.wellness->active_minutes != 45.0 ||
        fetched_well.wellness->step_count != 12000 || fetched_well.wellness->activity_type != "running") {
        std::cerr << "[Test] FAILED: Wellness fields mismatch" << std::endl;
        exit(1);
    }

    // Attempt to query wellness entry using Bob's UID -> Expect FAILURE / ACCESS DENIED
    auto buf_well_bob = store->get(key_well, bob_uid);
    if (buf_well_bob.size() > 0) {
        std::cerr << "[Test] FAILED: Intruder Bob was allowed to read Jason's wellness journal node" << std::endl;
        exit(1);
    }

    // 5. Query back education entry using Jason's UID -> Expect SUCCESS
    auto key_edu = std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("jasoncoposky:journal-edu-1")));
    auto buf_edu = store->get(key_edu, jason_uid);
    if (buf_edu.size() == 0) {
        std::cerr << "[Test] FAILED: Jason could not retrieve his own education journal node" << std::endl;
        exit(1);
    }
    CpbEntry fetched_edu = CpbEntry::deserialize(buf_edu);
    if (!fetched_edu.education) {
        std::cerr << "[Test] FAILED: Deserialized education block is missing" << std::endl;
        exit(1);
    }
    if (fetched_edu.education->institution_platform != "MIT OpenCourseWare" || fetched_edu.education->resource_type != "lecture" ||
        fetched_edu.education->progress_percent != 25.0 || fetched_edu.education->focus_duration_minutes != 60.0 ||
        fetched_edu.education->credential_uuid != "cred-algebra-101") {
        std::cerr << "[Test] FAILED: Education fields mismatch" << std::endl;
        exit(1);
    }

    // Attempt to query education entry using Bob's UID -> Expect FAILURE / ACCESS DENIED
    auto buf_edu_bob = store->get(key_edu, bob_uid);
    if (buf_edu_bob.size() > 0) {
        std::cerr << "[Test] FAILED: Intruder Bob was allowed to read Jason's education journal node" << std::endl;
        exit(1);
    }

    // 6. Test semantic link (MENTIONS & OCCURRED_AT)
    bb.get_engine()->add_edge("jasoncoposky:journal-well-1", rel::MENTIONS, 1.0, "identity:jasoncoposky");
    bb.get_engine()->add_edge("jasoncoposky:journal-well-1", rel::OCCURRED_AT, 1.0, "spatial-marker-nc");
    store->wait_all_shards();

    auto node_well = bb.get_engine()->get_node(bb.get_engine()->get_resolver().parse_uuid("jasoncoposky:journal-well-1"));
    auto mentions_edges = node_well->get_edges(rel::MENTIONS);
    if (mentions_edges.empty() || mentions_edges[0]->get_dst() != bb.get_engine()->get_resolver().parse_uuid("identity:jasoncoposky")) {
        std::cerr << "[Test] FAILED: Mentions relationship verification failed" << std::endl;
        exit(1);
    }

    auto location_edges = node_well->get_edges(rel::OCCURRED_AT);
    if (location_edges.empty() || location_edges[0]->get_dst() != bb.get_engine()->get_resolver().parse_uuid("spatial-marker-nc")) {
        std::cerr << "[Test] FAILED: Occurred At relationship verification failed" << std::endl;
        exit(1);
    }

    std::cout << "[Test] Life Journaling Verification PASSED" << std::endl;
}

void test_commonplace_note_references() {
    std::cout << "[Test] Starting Commonplace Note References Verification..." << std::endl;
    std::filesystem::remove_all("test_note_ref_db");
    {
        Blackboard bb("test_note_ref_db", 8);
        auto* store = bb.get_engine()->get_store();

        CpbEntry entry;
        entry.header.uuid = "note-ref-1";
        entry.header.origin.agent_id = "agent-ref";
        entry.header.origin.project_id = "project-ref";
        entry.taxonomy.knowledge_area = KnowledgeArea::LITERATURE_READING;
        entry.payload.statement = "SICP and Lisp Foundations";

        Reference ref1;
        ref1.title = "Structure and Interpretation of Computer Programs";
        ref1.page_numbers = "pp. 359-380";
        ref1.creator = "Harold Abelson";
        ref1.tags = {"lisp", "metalinguistic"};
        ref1.excerpt = "The evaluator, which determines the meaning of expressions...";
        entry.payload.references.push_back(ref1);

        Reference ref2;
        ref2.title = "Recursive Functions of Symbolic Expressions";
        ref2.page_numbers = "CACM 3(4)";
        ref2.creator = "John McCarthy";
        ref2.tags = {"lisp", "lambda-calculus"};
        ref2.uuid = "urn:doi:10.1145/367177.367199";
        entry.payload.references.push_back(ref2);

        bb.commit_cpb_entry(entry);
        store->wait_all_shards();

        auto key = std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("note-ref-1")));
        auto raw_buf = store->get(key);
        if (raw_buf.size() == 0) {
            std::cerr << "[Test] FAILED: Could not retrieve note-ref-1 from store" << std::endl;
            exit(1);
        }

        CpbEntry fetched = CpbEntry::deserialize(raw_buf);
        if (fetched.payload.references.size() != 2) {
            std::cerr << "[Test] FAILED: Expected 2 references, got " << fetched.payload.references.size() << std::endl;
            exit(1);
        }

        const auto& r1 = fetched.payload.references[0];
        if (r1.title != "Structure and Interpretation of Computer Programs" ||
            r1.page_numbers != "pp. 359-380" ||
            r1.creator != "Harold Abelson" ||
            r1.tags != std::vector<std::string>{"lisp", "metalinguistic"} ||
            r1.excerpt != "The evaluator, which determines the meaning of expressions...") {
            std::cerr << "[Test] FAILED: Reference 1 fields mismatch" << std::endl;
            exit(1);
        }

        const auto& r2 = fetched.payload.references[1];
        if (r2.title != "Recursive Functions of Symbolic Expressions" ||
            r2.page_numbers != "CACM 3(4)" ||
            r2.creator != "John McCarthy" ||
            r2.tags != std::vector<std::string>{"lisp", "lambda-calculus"} ||
            r2.uuid != "urn:doi:10.1145/367177.367199") {
            std::cerr << "[Test] FAILED: Reference 2 fields mismatch" << std::endl;
            exit(1);
        }

        auto outbound = bb.get_outbound_links("note-ref-1");
        bool has_doi_cites = false;
        for (const auto& [dst, rel] : outbound) {
            if (dst == "urn:doi:10.1145/367177.367199" && rel == rel::CITES) {
                has_doi_cites = true;
            }
        }
        if (!has_doi_cites) {
            std::cerr << "[Test] FAILED: Outbound links for note-ref-1 missing (urn:doi:10.1145/367177.367199, CITES)" << std::endl;
            exit(1);
        }
    }
    std::filesystem::remove_all("test_note_ref_db");
    std::cout << "[Test] Commonplace Note References Verification PASSED" << std::endl;
}

void test_note_graph_backlinks() {
    std::cout << "[Test] Starting Note Graph Backlinks Verification..." << std::endl;
    std::filesystem::remove_all("test_backlinks_db");
    {
        Blackboard bb("test_backlinks_db", 9);
        auto* store = bb.get_engine()->get_store();

        bb.register_user_credentials("alice", "alice-key");
        uint32_t alice_uid = bb.get_user_uid("alice");

        bb.register_user_credentials("intruder-agent", "intruder-key");
        uint32_t intruder_uid = bb.get_user_uid("intruder-agent");

        CpbEntry note_a;
        note_a.header.uuid = "note-A";
        note_a.header.origin.agent_id = "identity:alice";
        note_a.header.origin.project_id = "project-backlinks";
        note_a.payload.statement = "Note A on Foundations";

        CpbEntry note_b;
        note_b.header.uuid = "note-B";
        note_b.header.origin.agent_id = "identity:alice";
        note_b.header.origin.project_id = "project-backlinks";
        note_b.payload.statement = "Note B on Recursion";
        NoteLink link_b;
        link_b.target_uuid = "note-A";
        link_b.relation = rel::EXTENDS;
        link_b.context = "Extends Note A on recursion";
        note_b.payload.note_links.push_back(link_b);

        CpbEntry note_c;
        note_c.header.uuid = "note-C";
        note_c.header.origin.agent_id = "identity:alice";
        note_c.header.origin.project_id = "project-backlinks";
        note_c.payload.statement = "Note C related concepts";
        NoteLink link_c;
        link_c.target_uuid = "note-A";
        link_c.relation = ""; // empty relation defaults to rel::SEE_ALSO
        link_c.context = "Related context";
        note_c.payload.note_links.push_back(link_c);

        bb.commit_cpb_entry(note_a, alice_uid);
        bb.commit_cpb_entry(note_b, alice_uid);
        bb.commit_cpb_entry(note_c, alice_uid);
        store->wait_all_shards();

        // Multi-tenant isolation: intruder must not see Note A backlinks
        auto intruder_backlinks = bb.get_backlinks("note-A", intruder_uid);
        if (!intruder_backlinks.empty()) {
            std::cerr << "[Test] FAILED: Multi-tenant isolation failure: intruder accessed note-A backlinks" << std::endl;
            exit(1);
        }

        auto backlinks = bb.get_backlinks("note-A", alice_uid);
        bool has_b_extends = false;
        bool has_c_see_also = false;
        for (const auto& [src, rel] : backlinks) {
            if (src == "note-B" && rel == "EXTENDS") has_b_extends = true;
            if (src == "note-C" && rel == "SEE_ALSO") has_c_see_also = true;
        }

        if (!has_b_extends) {
            std::cerr << "[Test] FAILED: Backlinks for note-A missing (note-B, EXTENDS)" << std::endl;
            exit(1);
        }
        if (!has_c_see_also) {
            std::cerr << "[Test] FAILED: Backlinks for note-A missing (note-C, SEE_ALSO)" << std::endl;
            exit(1);
        }

        auto outbound_b = bb.get_outbound_links("note-B", alice_uid);
        bool has_a_extends = false;
        for (const auto& [dst, rel] : outbound_b) {
            if (dst == "note-A" && rel == "EXTENDS") has_a_extends = true;
        }

        if (!has_a_extends) {
            std::cerr << "[Test] FAILED: Outbound links for note-B missing (note-A, EXTENDS)" << std::endl;
            exit(1);
        }
    }
    std::filesystem::remove_all("test_backlinks_db");
    std::cout << "[Test] Note Graph Backlinks Verification PASSED" << std::endl;
}

void test_universal_catalog_recipe() {
    std::cout << "[Test] Starting Universal Catalog Recipe Verification..." << std::endl;
    std::filesystem::remove_all("test_recipe_db");
    {
        Blackboard bb("test_recipe_db", 10);
        auto* store = bb.get_engine()->get_store();

        CpbEntry recipe;
        recipe.header.uuid = "recipe-tiramisu";
        recipe.header.origin.agent_id = "chef-luigi";
        recipe.header.origin.project_id = "project-cookbook";
        recipe.taxonomy.knowledge_area = KnowledgeArea::CULINARY_RECIPES;
        recipe.payload.statement = "Classic Tiramisu";

        recipe.items.push_back({ "Mascarpone", 500.0, "g", "INGREDIENT", "room temperature" });
        recipe.items.push_back({ "Ladyfingers", 30.0, "pcs", "INGREDIENT", "Savoiardi" });
        recipe.items.push_back({ "Espresso", 250.0, "ml", "INGREDIENT", "freshly brewed" });

        recipe.steps.push_back({ 1, "Brew espresso and allow to cool", 300, "" });
        recipe.steps.push_back({ 2, "Whisk egg yolks with sugar, fold in mascarpone", 600, "" });
        recipe.steps.push_back({ 3, "Dip ladyfingers in espresso and layer with cream", 450, "" });

        recipe.metrics.push_back({ "prep_time", 30.0, "min" });
        recipe.metrics.push_back({ "servings", 8.0, "yield" });
        recipe.metrics.push_back({ "calories", 420.0, "kcal" });

        recipe.attributes["cuisine"] = "Italian";
        recipe.attributes["course"] = "DESSERT";

        bb.commit_cpb_entry(recipe);
        store->wait_all_shards();

        auto key = std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("recipe-tiramisu")));
        auto raw_buf = store->get(key);
        if (raw_buf.size() == 0) {
            std::cerr << "[Test] FAILED: Could not retrieve recipe-tiramisu from store" << std::endl;
            exit(1);
        }

        CpbEntry fetched = CpbEntry::deserialize(raw_buf);

        if (fetched.items.size() != 3) {
            std::cerr << "[Test] FAILED: Expected 3 items, got " << fetched.items.size() << std::endl;
            exit(1);
        }
        if (fetched.items[0].name != "Mascarpone" || fetched.items[0].quantity != 500.0 ||
            fetched.items[0].unit != "g" || fetched.items[0].role != "INGREDIENT" ||
            fetched.items[0].notes != "room temperature") {
            std::cerr << "[Test] FAILED: Item 0 mismatch" << std::endl;
            exit(1);
        }
        if (fetched.items[1].name != "Ladyfingers" || fetched.items[1].quantity != 30.0 ||
            fetched.items[1].unit != "pcs" || fetched.items[1].role != "INGREDIENT" ||
            fetched.items[1].notes != "Savoiardi") {
            std::cerr << "[Test] FAILED: Item 1 mismatch" << std::endl;
            exit(1);
        }
        if (fetched.items[2].name != "Espresso" || fetched.items[2].quantity != 250.0 ||
            fetched.items[2].unit != "ml" || fetched.items[2].role != "INGREDIENT" ||
            fetched.items[2].notes != "freshly brewed") {
            std::cerr << "[Test] FAILED: Item 2 mismatch" << std::endl;
            exit(1);
        }

        if (fetched.steps.size() != 3) {
            std::cerr << "[Test] FAILED: Expected 3 steps, got " << fetched.steps.size() << std::endl;
            exit(1);
        }
        if (fetched.steps[0].step_number != 1 || fetched.steps[0].instruction != "Brew espresso and allow to cool" ||
            fetched.steps[0].duration_seconds != 300 || fetched.steps[0].notes != "") {
            std::cerr << "[Test] FAILED: Step 0 mismatch" << std::endl;
            exit(1);
        }
        if (fetched.steps[1].step_number != 2 || fetched.steps[1].instruction != "Whisk egg yolks with sugar, fold in mascarpone" ||
            fetched.steps[1].duration_seconds != 600 || fetched.steps[1].notes != "") {
            std::cerr << "[Test] FAILED: Step 1 mismatch" << std::endl;
            exit(1);
        }
        if (fetched.steps[2].step_number != 3 || fetched.steps[2].instruction != "Dip ladyfingers in espresso and layer with cream" ||
            fetched.steps[2].duration_seconds != 450 || fetched.steps[2].notes != "") {
            std::cerr << "[Test] FAILED: Step 2 mismatch" << std::endl;
            exit(1);
        }

        if (fetched.metrics.size() != 3) {
            std::cerr << "[Test] FAILED: Expected 3 metrics, got " << fetched.metrics.size() << std::endl;
            exit(1);
        }
        if (fetched.metrics[0].key != "prep_time" || fetched.metrics[0].value != 30.0 || fetched.metrics[0].unit != "min") {
            std::cerr << "[Test] FAILED: Metric 0 mismatch" << std::endl;
            exit(1);
        }
        if (fetched.metrics[1].key != "servings" || fetched.metrics[1].value != 8.0 || fetched.metrics[1].unit != "yield") {
            std::cerr << "[Test] FAILED: Metric 1 mismatch" << std::endl;
            exit(1);
        }
        if (fetched.metrics[2].key != "calories" || fetched.metrics[2].value != 420.0 || fetched.metrics[2].unit != "kcal") {
            std::cerr << "[Test] FAILED: Metric 2 mismatch" << std::endl;
            exit(1);
        }

        if (fetched.attributes.count("cuisine") == 0 || fetched.attributes["cuisine"] != "Italian") {
            std::cerr << "[Test] FAILED: Attribute cuisine mismatch" << std::endl;
            exit(1);
        }
        if (fetched.attributes.count("course") == 0 || fetched.attributes["course"] != "DESSERT") {
            std::cerr << "[Test] FAILED: Attribute course mismatch" << std::endl;
            exit(1);
        }
    }
    std::filesystem::remove_all("test_recipe_db");
    std::cout << "[Test] Universal Catalog Recipe Verification PASSED" << std::endl;
}

void test_rdf_export_notes_and_recipes() {
    std::cout << "[Test] Starting RDF Export Notes and Recipes Verification..." << std::endl;
    std::filesystem::remove_all("test_rdf_export_db");
    {
        Blackboard bb("test_rdf_export_db", 11);
        auto* store = bb.get_engine()->get_store();

        // 1. Identity
        IdentityNode identity;
        identity.id = "identity:doug";
        identity.display_name = "Douglas Hofstadter";
        identity.role = "Author";
        bb.commit_identity_node(identity);

        // 2. Project
        ProjectNode project;
        project.project_id = "project:commonplace";
        project.description = "Life-long Commonplace Book";
        project.lifecycle_status = "ACTIVE";
        bb.commit_project_node(project);

        // 3. Note
        CpbEntry note;
        note.header.uuid = "note-ref-1";
        note.header.origin.agent_id = "identity:doug";
        note.header.origin.project_id = "project:commonplace";
        note.taxonomy.knowledge_area = KnowledgeArea::LITERATURE_READING;
        note.payload.statement = "Gödel, Escher, Bach: Strange Loops";

        Reference ref;
        ref.title = "Gödel, Escher, Bach: an Eternal Golden Braid";
        ref.creator = "Douglas Hofstadter";
        ref.page_numbers = "pp. 1-742";
        ref.uuid = "urn:isbn:0465026567";
        note.payload.references.push_back(ref);

        NoteLink link;
        link.target_uuid = "note-foundation";
        link.relation = rel::EXTENDS;
        link.context = "Extends foundation note";
        note.payload.note_links.push_back(link);

        bb.commit_cpb_entry(note);

        // 4. Recipe
        CpbEntry recipe;
        recipe.header.uuid = "recipe-tiramisu";
        recipe.header.origin.agent_id = "identity:doug";
        recipe.header.origin.project_id = "project:commonplace";
        recipe.taxonomy.knowledge_area = KnowledgeArea::CULINARY_RECIPES;
        recipe.payload.statement = "Classic Tiramisu";
        recipe.items.push_back({ "Mascarpone", 500.0, "g", "INGREDIENT", "room temperature" });
        recipe.steps.push_back({ 1, "Brew espresso and allow to cool", 300, "" });
        recipe.metrics.push_back({ "prep_time", 30.0, "min" });
        recipe.metrics.push_back({ "servings", 8.0, "yield" });
        recipe.attributes["cuisine"] = "Italian";

        bb.commit_cpb_entry(recipe);
        store->wait_all_shards();

        std::string ttl = RdfExporter::export_turtle(&bb);

        std::vector<std::string> required_strings = {
            "@prefix schema: <http://schema.org/>",
            "@prefix dc: <http://purl.org/dc/terms/>",
            "schema:CreativeWork",
            "schema:citation",
            "schema:recipeIngredient",
            "schema:recipeInstructions",
            "asos:extends",
            "dc:title",
            "schema:Recipe",
            "asos:metric",
            "asos:attribute"
        };

        for (const auto& req : required_strings) {
            if (ttl.find(req) == std::string::npos) {
                std::cerr << "[Test] FAILED: RDF export missing expected substring: " << req << std::endl;
                std::cerr << "Exported Turtle:\n" << ttl << std::endl;
                exit(1);
            }
        }
    }
    std::filesystem::remove_all("test_rdf_export_db");
    std::cout << "[Test] RDF Export Notes and Recipes Verification PASSED" << std::endl;
}

void test_api_search_and_links() {
    std::cout << "\n[Test] Starting API Search and Node Links Verification..." << std::endl;
    std::string db_dir = "test_api_search_db";
    std::filesystem::remove_all(db_dir);

#define ASOS_CHECK(cond) do { if (!(cond)) { std::cerr << "[Test] FAILED: " #cond " at line " << __LINE__ << std::endl; exit(1); } } while(0)

    {
        Blackboard bb(db_dir, 12);
        auto* store = bb.get_engine()->get_store();

        // Commit two linked notes
        CpbEntry note1;
        note1.header.uuid = "note-search-1";
        note1.header.origin.project_id = "proj-search";
        note1.header.origin.agent_id = "agent-search";
        note1.payload.statement = "Searchable Strange Loops in Cognitive Science";
        note1.taxonomy.tags = {"COGNITION", "LOOPS"};
        note1.taxonomy.knowledge_area = KnowledgeArea::LITERATURE_READING;
        Reference ref1;
        ref1.title = "Gödel, Escher, Bach";
        ref1.creator = "Douglas Hofstadter";
        note1.payload.references.push_back(ref1);
        ASOS_CHECK(bb.commit_cpb_entry(note1));

        CpbEntry note2;
        note2.header.uuid = "note-search-2";
        note2.header.origin.project_id = "proj-search";
        note2.header.origin.agent_id = "agent-search";
        note2.payload.statement = "Recursion in Neural Computation";
        note2.taxonomy.tags = {"NEURAL", "LOOPS"};
        note2.taxonomy.knowledge_area = KnowledgeArea::COMPUTING_FOUNDATIONS;
        NoteLink link;
        link.target_uuid = "note-search-1";
        link.relation = rel::EXTENDS;
        link.context = "Builds upon strange loop cognitive architectures";
        note2.payload.note_links.push_back(link);
        ASOS_CHECK(bb.commit_cpb_entry(note2));

        store->wait_all_shards();

        // Verify Blackboard backlinks & outbound
        auto backlinks = bb.get_backlinks("note-search-1");
        ASOS_CHECK(backlinks.size() == 1);
        ASOS_CHECK(backlinks[0].first == "note-search-2");
        ASOS_CHECK(backlinks[0].second == rel::EXTENDS);

        auto outbound = bb.get_outbound_links("note-search-2");
        ASOS_CHECK(outbound.size() >= 1);
        bool found_outbound = false;
        for (auto& [dst, r] : outbound) {
            if (dst == "note-search-1" && r == rel::EXTENDS) found_outbound = true;
        }
        ASOS_CHECK(found_outbound);

        // Start ApiServer
        int test_port = 18085;
        ApiServer::instance().start(&bb, test_port);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        httplib::Client cli("127.0.0.1", test_port);

        // 1. GET /api/v1/schema
        auto res_schema = cli.Get("/api/v1/schema");
        ASOS_CHECK(res_schema && res_schema->status == 200);
        auto schema_json = nlohmann::json::parse(res_schema->body);
        ASOS_CHECK(schema_json.contains("knowledge_areas"));
        ASOS_CHECK(schema_json.contains("relationships"));
        ASOS_CHECK(schema_json.contains("types"));
        ASOS_CHECK(schema_json["types"].contains("CPB_ENTRY"));
        ASOS_CHECK(schema_json["types"].contains("REFERENCE"));
        ASOS_CHECK(schema_json["types"].contains("NOTE_LINK"));
        ASOS_CHECK(schema_json["types"].contains("CATALOG_ITEM"));
        ASOS_CHECK(schema_json["types"].contains("CATALOG_STEP"));
        ASOS_CHECK(schema_json["types"].contains("CATALOG_METRIC"));

        // 2. GET /api/v1/search
        auto res_search = cli.Get("/api/v1/search?q=Strange");
        ASOS_CHECK(res_search && res_search->status == 200);
        auto search_json = nlohmann::json::parse(res_search->body);
        ASOS_CHECK(search_json["count"].get<int>() >= 1);
        ASOS_CHECK(search_json["matches"][0]["uuid"] == "note-search-1");

        // Search by tag
        auto res_tag_search = cli.Get("/api/v1/search?q=&tags=NEURAL");
        ASOS_CHECK(res_tag_search && res_tag_search->status == 200);
        auto tag_json = nlohmann::json::parse(res_tag_search->body);
        ASOS_CHECK(tag_json["count"].get<int>() >= 1);
        ASOS_CHECK(tag_json["matches"][0]["uuid"] == "note-search-2");

        // 3. GET /api/v1/node/:id/links
        auto res_links = cli.Get("/api/v1/node/note-search-1/links?direction=both");
        ASOS_CHECK(res_links && res_links->status == 200);
        auto links_json = nlohmann::json::parse(res_links->body);
        ASOS_CHECK(links_json["uuid"] == "note-search-1");
        ASOS_CHECK(links_json["inbound"].size() == 1);
        ASOS_CHECK(links_json["inbound"][0]["source"] == "note-search-2");
        ASOS_CHECK(links_json["inbound"][0]["relation"] == rel::EXTENDS);
        ASOS_CHECK(links_json["inbound"][0]["statement"] == "Recursion in Neural Computation");

        // 4. POST /api/v1/graph/bundle (rich deserialization)
        nlohmann::json bundle = {
            {"project_id", "proj-bundle"},
            {"agent_id", "agent-bundle"},
            {"atoms", nlohmann::json::array({
                {
                    {"uuid", "recipe-bundle-1"},
                    {"statement", "Rich Bundled Espresso Torta"},
                    {"ka", 27},
                    {"tags", {"RECIPE", "DESSERT"}},
                    {"references", nlohmann::json::array({
                        {
                            {"title", "Italian Baking Classics"},
                            {"creator", "Nonna Rosa"},
                            {"page_numbers", "p. 42"},
                            {"uuid", "urn:isbn:123456"}
                        }
                    })},
                    {"note_links", nlohmann::json::array({
                        {
                            {"target_uuid", "note-search-1"},
                            {"relation", "PAIRS_WITH"},
                            {"context", "Great reading companion"}
                        }
                    })},
                    {"items", nlohmann::json::array({
                        {
                            {"name", "Dark Chocolate"},
                            {"quantity", 200.0},
                            {"unit", "g"},
                            {"role", "INGREDIENT"},
                            {"notes", "70% cocoa"}
                        }
                    })},
                    {"steps", nlohmann::json::array({
                        {
                            {"step_number", 1},
                            {"instruction", "Melt chocolate in bain-marie"},
                            {"duration_minutes", 5.0}
                        }
                    })},
                    {"metrics", nlohmann::json::array({
                        {
                            {"name", "baking_temp"},
                            {"value", 180.0},
                            {"unit", "C"}
                        }
                    })},
                    {"attributes", {
                        {"difficulty", "Easy"}
                    }}
                }
            })}
        };

        auto res_bundle = cli.Post("/api/v1/graph/bundle", bundle.dump(), "application/json");
        ASOS_CHECK(res_bundle && res_bundle->status == 200);

        store->wait_all_shards();

        // Verify bundle created the rich atom
        auto res_get_node = cli.Get("/api/v1/node/recipe-bundle-1");
        ASOS_CHECK(res_get_node && res_get_node->status == 200);
        auto node_json = nlohmann::json::parse(res_get_node->body);
        ASOS_CHECK(node_json["uuid"] == "recipe-bundle-1");
        ASOS_CHECK(node_json["statement"] == "Rich Bundled Espresso Torta");
        ASOS_CHECK(node_json["items"].size() == 1);
        ASOS_CHECK(node_json["items"][0]["name"] == "Dark Chocolate");
        ASOS_CHECK(node_json["steps"].size() == 1);
        ASOS_CHECK(node_json["steps"][0]["step_number"] == 1);
        ASOS_CHECK(node_json["metrics"].size() == 1);
        ASOS_CHECK(node_json["metrics"][0]["name"] == "baking_temp");
        ASOS_CHECK(node_json["attributes"]["difficulty"] == "Easy");

        // 4b. Test POST /api/v1/graph/bundle error handling (missing/invalid atoms array)
        nlohmann::json bad_bundle = {
            {"project_id", "proj-bundle"},
            {"agent_id", "agent-bundle"}
        };
        auto res_bad_bundle = cli.Post("/api/v1/graph/bundle", bad_bundle.dump(), "application/json");
        ASOS_CHECK(res_bad_bundle && res_bad_bundle->status == 400);
        ASOS_CHECK(res_bad_bundle->body == "Error: Missing atoms array");

        // 5. Multi-Tenancy ACL Verification
        // Commit a tenant-isolated note with X-Active-User: tenant-alice
        nlohmann::json alice_bundle = {
            {"project_id", "proj-alice"},
            {"agent_id", "tenant-alice"},
            {"atoms", nlohmann::json::array({
                {
                    {"uuid", "note-alice-secret-1"},
                    {"statement", "Alice Secret Confidential Cognitive Architecture"},
                    {"ka", 1},
                    {"tags", {"ALICE_ONLY", "CONFIDENTIAL"}}
                }
            })}
        };
        httplib::Headers alice_headers = {{"X-Active-User", "tenant-alice"}};
        auto res_alice = cli.Post("/api/v1/graph/bundle", alice_headers, alice_bundle.dump(), "application/json");
        ASOS_CHECK(res_alice && res_alice->status == 200);

        store->wait_all_shards();

        // Bob queries GET /api/v1/search with X-Active-User: tenant-bob
        httplib::Headers bob_headers = {{"X-Active-User", "tenant-bob"}};
        auto res_bob_search = cli.Get("/api/v1/search?q=Confidential", bob_headers);
        ASOS_CHECK(res_bob_search && res_bob_search->status == 200);
        auto bob_search_json = nlohmann::json::parse(res_bob_search->body);
        for (const auto& match : bob_search_json["matches"]) {
            ASOS_CHECK(match["uuid"] != "note-alice-secret-1");
        }

        // Bob queries GET /api/v1/node/:id/links for Alice's note - assert 404 / access denied
        auto res_bob_links = cli.Get("/api/v1/node/note-alice-secret-1/links", bob_headers);
        ASOS_CHECK(res_bob_links && res_bob_links->status == 404);

        // Non-existent node query should also return 404 Not Found
        auto res_missing_links = cli.Get("/api/v1/node/nonexistent-node-12345/links");
        ASOS_CHECK(res_missing_links && res_missing_links->status == 404);

        // Alice queries GET /api/v1/node/:id/links for her note - should succeed (200)
        auto res_alice_links = cli.Get("/api/v1/node/note-alice-secret-1/links", alice_headers);
        ASOS_CHECK(res_alice_links && res_alice_links->status == 200);

        ApiServer::instance().stop();
    }
#undef ASOS_CHECK
    std::filesystem::remove_all(db_dir);
    std::cout << "[Test] API Search and Node Links Verification PASSED" << std::endl;
}

void test_multi_surface_provenance(Blackboard& bb) {
    std::cout << "[Test] Starting Multi-Surface Origin Provenance Verification..." << std::endl;
    auto* store = bb.get_engine()->get_store();

    CpbEntry atom;
    atom.header.uuid = "atom-provenance-1";
    atom.header.origin.user_id = "user:jason";
    atom.header.origin.agent_id = "agent:spatial-librarian";
    atom.header.origin.surface_id = "surface:multitouch-table-01";
    atom.header.origin.surface_type = "tabletop";
    atom.header.origin.project_id = "proj-quantum-optics";
    atom.header.origin.context_id = "ctx:lab-session-42";
    atom.header.origin.session_id = "sess-alpha-99";
    atom.payload.statement = "Multi-surface provenance test atom";

    bool committed = bb.commit_cpb_entry(atom);
    if (!committed) {
        std::cerr << "[Test] FAILED: Could not commit multi-surface provenance atom" << std::endl;
        exit(1);
    }
    store->wait_all_shards();

    auto key = std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("atom-provenance-1")));
    auto raw_buf = store->get(key);
    if (raw_buf.size() == 0) {
        std::cerr << "[Test] FAILED: Could not retrieve atom-provenance-1 from store" << std::endl;
        exit(1);
    }

    CpbEntry fetched = CpbEntry::deserialize(raw_buf);
    if (fetched.header.origin.user_id != "user:jason" ||
        fetched.header.origin.agent_id != "agent:spatial-librarian" ||
        fetched.header.origin.surface_id != "surface:multitouch-table-01" ||
        fetched.header.origin.surface_type != "tabletop" ||
        fetched.header.origin.project_id != "proj-quantum-optics" ||
        fetched.header.origin.context_id != "ctx:lab-session-42" ||
        fetched.header.origin.session_id != "sess-alpha-99") {
        std::cerr << "[Test] FAILED: Origin fields mismatch in multi-surface provenance test" << std::endl;
        exit(1);
    }
    // 1. Assert that the atom has CREATED_BY edges to both user:jason and agent:spatial-librarian
    auto atom_node = bb.get_engine()->get_node("atom-provenance-1");
    if (!atom_node) {
        std::cerr << "[Test] FAILED: atom-provenance-1 node not found in engine" << std::endl;
        exit(1);
    }
    bool has_user_edge = false;
    bool has_agent_edge = false;
    for (auto& edge : atom_node->get_edges(rel::CREATED_BY)) {
        if (edge->get_dst() == bb.get_engine()->get_resolver().parse_uuid("user:jason")) {
            has_user_edge = true;
        }
        if (edge->get_dst() == bb.get_engine()->get_resolver().parse_uuid("agent:spatial-librarian")) {
            has_agent_edge = true;
        }
    }
    if (!has_user_edge || !has_agent_edge) {
        std::cerr << "[Test] FAILED: CREATED_BY edges missing for dual identity (user:jason="
                  << has_user_edge << ", agent:spatial-librarian=" << has_agent_edge << ")" << std::endl;
        exit(1);
    }

    // 2. Test an atom created with ONLY user_id (and no agent_id), verifying successful commit and edge creation
    CpbEntry user_only_atom;
    user_only_atom.header.uuid = "atom-provenance-user-only";
    user_only_atom.header.origin.user_id = "user:alice";
    user_only_atom.header.origin.agent_id = "";
    user_only_atom.header.origin.project_id = "proj-quantum-optics";
    user_only_atom.payload.statement = "User-only provenance test atom";

    bool user_atom_committed = bb.commit_cpb_entry(user_only_atom);
    if (!user_atom_committed) {
        std::cerr << "[Test] FAILED: Could not commit user-only provenance atom" << std::endl;
        exit(1);
    }
    store->wait_all_shards();

    auto user_atom_node = bb.get_engine()->get_node("atom-provenance-user-only");
    if (!user_atom_node) {
        std::cerr << "[Test] FAILED: atom-provenance-user-only node not found" << std::endl;
        exit(1);
    }
    bool has_user_only_edge = false;
    for (auto& edge : user_atom_node->get_edges(rel::CREATED_BY)) {
        if (edge->get_dst() == bb.get_engine()->get_resolver().parse_uuid("user:alice")) {
            has_user_only_edge = true;
        }
    }
    if (!has_user_only_edge) {
        std::cerr << "[Test] FAILED: CREATED_BY edge to user:alice missing for user-only atom" << std::endl;
        exit(1);
    }

    auto user_atom_key = std::string(l3kvg::KeyBuilder::node_key(bb.get_engine()->get_resolver().parse_uuid("atom-provenance-user-only")));
    auto user_atom_raw = store->get(user_atom_key);
    CpbEntry user_atom_fetched = CpbEntry::deserialize(user_atom_raw);
    if (user_atom_fetched.header.origin.user_id != "user:alice" ||
        !user_atom_fetched.header.origin.agent_id.empty()) {
        std::cerr << "[Test] FAILED: Deserialized user-only atom fields mismatch (agent_id should be empty)" << std::endl;
        exit(1);
    }

    // 3. Test rejection of an orphan atom (missing both user_id and agent_id, or missing project_id)
    // Case A: Missing both user_id and agent_id
    CpbEntry orphan_no_id;
    orphan_no_id.header.uuid = "orphan-no-id";
    orphan_no_id.header.origin.project_id = "proj-quantum-optics";
    orphan_no_id.payload.statement = "Orphan missing user and agent";
    if (bb.commit_cpb_entry(orphan_no_id)) {
        std::cerr << "[Test] FAILED: Orphan atom missing both user_id and agent_id was accepted!" << std::endl;
        exit(1);
    }

    // Case B: Missing project_id (with user_id)
    CpbEntry orphan_no_proj_user;
    orphan_no_proj_user.header.uuid = "orphan-no-proj-user";
    orphan_no_proj_user.header.origin.user_id = "user:jason";
    orphan_no_proj_user.payload.statement = "Orphan missing project with user";
    if (bb.commit_cpb_entry(orphan_no_proj_user)) {
        std::cerr << "[Test] FAILED: Orphan atom missing project_id was accepted!" << std::endl;
        exit(1);
    }

    // Case C: Missing project_id (with agent_id)
    CpbEntry orphan_no_proj_agent;
    orphan_no_proj_agent.header.uuid = "orphan-no-proj-agent";
    orphan_no_proj_agent.header.origin.agent_id = "agent:spatial-librarian";
    orphan_no_proj_agent.payload.statement = "Orphan missing project with agent";
    if (bb.commit_cpb_entry(orphan_no_proj_agent)) {
        std::cerr << "[Test] FAILED: Orphan atom missing project_id with agent was accepted!" << std::endl;
        exit(1);
    }

    // 4. Test authorization logic with principal_id (user impersonation prevention)
    bb.register_user_credentials("jason", "jason-key");
    uint32_t jason_uid = bb.get_user_uid("jason");
    bb.register_user_credentials("intruder", "intruder-key");
    uint32_t intruder_uid = bb.get_user_uid("intruder");

    CpbEntry secure_atom;
    secure_atom.header.uuid = "atom-auth-test";
    secure_atom.header.origin.user_id = "user:jason";
    secure_atom.header.origin.agent_id = "agent:intruder";
    secure_atom.header.origin.project_id = "proj-quantum-optics";
    secure_atom.payload.statement = "Secure authorization test atom";

    // Intruder principal trying to commit on behalf of user:jason must be rejected
    if (bb.commit_cpb_entry(secure_atom, intruder_uid)) {
        std::cerr << "[Test] FAILED: Intruder principal successfully committed atom on behalf of user:jason!" << std::endl;
        exit(1);
    }

    // Legitimate user principal committing user:jason must succeed
    if (!bb.commit_cpb_entry(secure_atom, jason_uid)) {
        std::cerr << "[Test] FAILED: Legitimate user principal failed to commit atom" << std::endl;
        exit(1);
    }

    // 5. Test deserialization robustness against buffers without origin object
    lite3cpp::Buffer legacy_buf;
    legacy_buf.init_object();
    size_t leg_h = legacy_buf.set_obj(0, "header");
    legacy_buf.set_str(leg_h, "uuid", "legacy-atom-no-origin");
    legacy_buf.set_i64(leg_h, "timestamp", 123456);
    size_t leg_t = legacy_buf.set_obj(0, "taxonomy");
    legacy_buf.set_i64(leg_t, "knowledge_area", 0);
    legacy_buf.set_i64(leg_t, "applicability", 100);
    legacy_buf.set_bool(leg_t, "uncertainty", false);
    legacy_buf.set_bool(leg_t, "is_principle", false);
    legacy_buf.set_arr(leg_t, "tags");
    size_t leg_p = legacy_buf.set_obj(0, "payload");
    legacy_buf.set_str(leg_p, "content_type", "text/markdown");
    legacy_buf.set_str(leg_p, "statement", "Legacy statement");
    legacy_buf.set_str(leg_p, "content", "Legacy content");
    legacy_buf.set_arr(leg_p, "artifact_refs");
    legacy_buf.set_arr(leg_p, "references");
    legacy_buf.set_arr(leg_p, "note_links");
    legacy_buf.set_arr(0, "items");
    legacy_buf.set_arr(0, "steps");
    legacy_buf.set_arr(0, "metrics");
    legacy_buf.set_obj(0, "attributes");

    CpbEntry legacy_entry = CpbEntry::deserialize(legacy_buf);
    if (legacy_entry.header.uuid != "legacy-atom-no-origin" || !legacy_entry.header.origin.user_id.empty() || !legacy_entry.header.origin.agent_id.empty()) {
        std::cerr << "[Test] FAILED: Deserialization of buffer without origin object failed or produced non-empty fields" << std::endl;
        exit(1);
    }

    std::cout << "[Test] Multi-Surface Origin Provenance Verification PASSED" << std::endl;
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
        test_life_journaling();
        test_commonplace_note_references();
        test_note_graph_backlinks();
        test_universal_catalog_recipe();
        test_rdf_export_notes_and_recipes();
        test_api_search_and_links();
        std::filesystem::remove_all("test_provenance_db");
        {
            Blackboard bb("test_provenance_db", 13);
            test_multi_surface_provenance(bb);
        }
        std::filesystem::remove_all("test_provenance_db");
        std::cout << "\n[SUCCESS] All ASOS Verification Tests Passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[Test] ERROR: Catch-all exception: " << e.what() << std::endl;
        return 1;
    }
}

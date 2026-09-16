#include "asos/Blackboard.hpp"
#include "asos/Orchestrator.hpp"
#include "asos/Librarian.hpp"
#include "asos/Validator.hpp"
#include "asos/DeltaEngine.hpp"
#include "asos/Monitor.hpp"
#include "asos/RdfExporter.hpp"
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

        CpbEntry note_a;
        note_a.header.uuid = "note-A";
        note_a.header.origin.agent_id = "agent-backlinks";
        note_a.header.origin.project_id = "project-backlinks";
        note_a.payload.statement = "Note A on Foundations";

        CpbEntry note_b;
        note_b.header.uuid = "note-B";
        note_b.header.origin.agent_id = "agent-backlinks";
        note_b.header.origin.project_id = "project-backlinks";
        note_b.payload.statement = "Note B on Recursion";
        NoteLink link_b;
        link_b.target_uuid = "note-A";
        link_b.relation = rel::EXTENDS;
        link_b.context = "Extends Note A on recursion";
        note_b.payload.note_links.push_back(link_b);

        CpbEntry note_c;
        note_c.header.uuid = "note-C";
        note_c.header.origin.agent_id = "agent-backlinks";
        note_c.header.origin.project_id = "project-backlinks";
        note_c.payload.statement = "Note C related concepts";
        NoteLink link_c;
        link_c.target_uuid = "note-A";
        link_c.relation = ""; // empty relation defaults to rel::SEE_ALSO
        link_c.context = "Related context";
        note_c.payload.note_links.push_back(link_c);

        bb.commit_cpb_entry(note_a);
        bb.commit_cpb_entry(note_b);
        bb.commit_cpb_entry(note_c);
        store->wait_all_shards();

        auto backlinks = bb.get_backlinks("note-A");
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

        auto outbound_b = bb.get_outbound_links("note-B");
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
            "dc:title"
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
        std::cout << "\n[SUCCESS] All ASOS Verification Tests Passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[Test] ERROR: Catch-all exception: " << e.what() << std::endl;
        return 1;
    }
}

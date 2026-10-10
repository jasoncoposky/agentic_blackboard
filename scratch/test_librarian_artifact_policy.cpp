#include "agentic_blackboard/Librarian.hpp"
#include "agentic_blackboard/Blackboard.hpp"
#include "agentic_blackboard/StorageManager.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>

int main() {
    std::string db_path = "/tmp/test_lib_policy_" + std::to_string(time(nullptr));
    std::filesystem::create_directories(db_path);

    blackboard::Blackboard bb(db_path + "/bb", 1);
    blackboard::storage::StorageManager sm(db_path + "/vault");
    blackboard::Librarian librarian(&bb);

    // Markdown text with YAML frontmatter
    std::string markdown = "---\n"
                           "title: CPG Swarm Coordination\n"
                           "license: SPDX:Apache-2.0\n"
                           "version: 2.1.0\n"
                           "---\n"
                           "# CPG Swarm Coordination\n\n"
                           "An industrial-grade autonomous swarm coordination skill.";

    auto processed = librarian.process_ingest_artifact(
        "/nucleus/specs/cpg.md",
        markdown,
        "blake3:fedcba9876543210",
        "user:jason",
        "agent:cpg-architect"
    );

    assert(processed.title == "CPG Swarm Coordination");
    assert(processed.license == "SPDX:Apache-2.0");
    assert(processed.version == "2.1.0");
    assert(processed.pid == "urn:ab:artifact:nucleus/specs/cpg.md");
    assert(!processed.uuid.empty());

    // Verify FAIR metrics score calculation
    double score = librarian.calculate_fair_score(processed);
    assert(score >= 80.0 && "Fully specified artifact must score >= 80 on FAIR rubric");

    // Verify AVU tag for fair:score was added
    bool has_fair_score_avu = false;
    for (const auto& avu : processed.avus) {
        if (avu.attribute == "fair:score") {
            has_fair_score_avu = true;
            assert(std::stod(avu.value) >= 80.0);
            assert(avu.units == "points");
        }
    }
    assert(has_fair_score_avu && "Must contain fair:score AVU");

    // Verify Blackboard integration (artifact committed to BB)
    auto retrieved = bb.get_artifact(processed.uuid);
    assert(retrieved.has_value() && "Processed artifact must be commited to blackboard");
    assert(retrieved->pid == processed.pid);

    // Verify heading fallback when frontmatter is absent
    std::string no_fm = "# Architecture Overview\n\nSystem description without YAML frontmatter.";
    auto processed2 = librarian.process_ingest_artifact(
        "/nucleus/docs/arch.json",
        no_fm,
        "blake3:1122334455667788"
    );
    assert(processed2.title == "Architecture Overview");
    assert(processed2.mime_type == "application/json");
    assert(processed2.pid == "urn:ab:artifact:nucleus/docs/arch.json");
    assert(processed2.collection_path == "/nucleus/docs");
    assert(processed2.logical_name == "arch.json");

    std::filesystem::remove_all(db_path);
    std::cout << "[SUCCESS] Librarian artifact policy hooks passed!" << std::endl;
    return 0;
}

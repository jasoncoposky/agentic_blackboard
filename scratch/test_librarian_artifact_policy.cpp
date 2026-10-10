#undef NDEBUG
#include <cassert>
#include "agentic_blackboard/Librarian.hpp"
#include "agentic_blackboard/Blackboard.hpp"
#include "agentic_blackboard/StorageManager.hpp"
#include <iostream>
#include <filesystem>
#include <sstream>
#include <algorithm>

int main() {
    std::string db_path = "/tmp/test_lib_policy_" + std::to_string(time(nullptr));
    std::filesystem::create_directories(db_path);

    blackboard::Blackboard bb(db_path + "/bb", 1);
    blackboard::storage::StorageManager sm(db_path + "/vault");
    blackboard::Librarian librarian(&bb);

    // 1. End-to-End Ingestion: Store markdown into CAS vault first
    std::string markdown = "---\n"
                           "title: CPG Swarm Coordination\n"
                           "license: SPDX:Apache-2.0\n"
                           "version: 2.1.0\n"
                           "---\n"
                           "# CPG Swarm Coordination\n\n"
                           "An industrial-grade autonomous swarm coordination skill.";

    std::istringstream in_stream(markdown);
    auto put_result = sm.store("default_posix_cas", in_stream).get();
    assert(!put_result.digest.empty() && "CAS store must return non-empty digest");
    assert(put_result.bytes_written == markdown.size());

    // 2. Process ingestion with initial AVUs (including forged fair:score to test deduplication)
    std::vector<agentic_blackboard::AVUTriple> initial_avus = {
        {"fair:score", "0", "fake"},
        {"domain", "swarm-robotics", ""}
    };

    auto processed = librarian.process_ingest_artifact(
        "/nucleus/specs/cpg.md",
        markdown,
        put_result.digest,
        "user:jason",
        "agent:cpg-architect",
        initial_avus
    );

    assert(processed.title == "CPG Swarm Coordination");
    assert(processed.license == "SPDX:Apache-2.0");
    assert(processed.version == "2.1.0");
    assert(processed.pid == "urn:ab:artifact:nucleus/specs/cpg.md");
    assert(!processed.uuid.empty());
    assert(processed.content_hash == put_result.digest);
    assert(processed.collection_path == "/nucleus/specs");
    assert(processed.logical_name == "cpg.md");

    // 3. Verify FAIR metrics score calculation
    double score = librarian.calculate_fair_score(processed);
    assert(score >= 80.0 && "Fully specified artifact must score >= 80 on FAIR rubric");

    // 4. Verify fair:score AVU was deduplicated (only 1 occurrence) and has genuine score
    int fair_score_count = 0;
    for (const auto& avu : processed.avus) {
        if (avu.attribute == "fair:score") {
            fair_score_count++;
            assert(std::stod(avu.value) == static_cast<double>(static_cast<int>(score)));
            assert(avu.units == "points");
        }
    }
    assert(fair_score_count == 1 && "Must contain exactly one deduplicated fair:score AVU");

    // Verify initial non-fair:score AVU preserved
    bool has_domain = false;
    for (const auto& avu : processed.avus) {
        if (avu.attribute == "domain" && avu.value == "swarm-robotics") {
            has_domain = true;
        }
    }
    assert(has_domain && "Must preserve initial domain AVU");

    // 5. Verify Blackboard integration (artifact committed to BB)
    auto retrieved = bb.get_artifact(processed.uuid);
    assert(retrieved.has_value() && "Processed artifact must be committed to blackboard");
    assert(retrieved->pid == processed.pid);
    assert(retrieved->content_hash == put_result.digest);

    // 6. Verify bb.query_by_avu("fair:score", ...) returns the artifact UUID
    std::string score_str = std::to_string(static_cast<int>(score));
    auto query_results = bb.query_by_avu("fair:score", score_str);
    assert(!query_results.empty() && "query_by_avu must return matches for fair:score");
    assert(std::find(query_results.begin(), query_results.end(), processed.uuid) != query_results.end() &&
           "Artifact UUID must be found in fair:score query results");

    auto domain_results = bb.query_by_avu("domain", "swarm-robotics");
    assert(std::find(domain_results.begin(), domain_results.end(), processed.uuid) != domain_results.end() &&
           "Artifact UUID must be found in domain query results");

    // 7. Verify heading fallback when frontmatter is absent
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

    // 8. Negative / Boundary Cases: Empty and slashes-only paths
    auto empty_entry = librarian.process_ingest_artifact(
        "",
        "Content with no logical path",
        "blake3:aabbcc"
    );
    assert(empty_entry.pid.empty() && "Empty path must yield empty PID");
    assert(empty_entry.collection_path.empty() && "Empty path must yield empty collection_path");
    assert(empty_entry.logical_name.empty() && "Empty path must yield empty logical_name");
    double empty_score = librarian.calculate_fair_score(empty_entry);
    assert(empty_score == 15.0 && "Empty path artifact with only content_hash must score 15");

    auto slashes_entry = librarian.process_ingest_artifact(
        "///",
        "Content with slashes-only path",
        "blake3:ddeeff"
    );
    assert(slashes_entry.pid.empty() && "Slashes-only path must yield empty PID");
    assert(slashes_entry.collection_path.empty() && "Slashes-only path must yield empty collection_path");
    assert(slashes_entry.logical_name.empty() && "Slashes-only path must yield empty logical_name");
    double slashes_score = librarian.calculate_fair_score(slashes_entry);
    assert(slashes_score == 15.0 && "Slashes-only path artifact must score 15");

    auto dotdot_entry = librarian.process_ingest_artifact(
        "../../escaped.md",
        "Content with traversal path",
        "blake3:escape"
    );
    assert(dotdot_entry.pid.empty() && "Traversal path must yield empty PID");
    assert(dotdot_entry.collection_path.empty() && "Traversal path must yield empty collection_path");
    assert(dotdot_entry.logical_name.empty() && "Traversal path must yield empty logical_name");

    auto dotdot_only = librarian.process_ingest_artifact(
        "..",
        "Content with parent dir path",
        "blake3:dotdot"
    );
    assert(dotdot_only.pid.empty() && "Parent dir path must yield empty PID");

    // 9. Boundary Case: Frontmatter with inline comments (outside and inside quotes)
    std::string markdown_comments =
        "---\n"
        "title: \"CPG Swarm Coordination #1\" # primary title with inline comment\n"
        "license: SPDX:Apache-2.0#custom-suffix # permissive license\n"
        "version: '2.1.0' # semantic version\n"
        "abstract: Swarm coordination skill # brief abstract\n"
        "---\n"
        "# Heading\n\nContent.";
    auto commented_entry = librarian.process_ingest_artifact(
        "/nucleus/specs/cpg_comments.md",
        markdown_comments,
        "blake3:commenthash"
    );
    assert(commented_entry.title == "CPG Swarm Coordination #1");
    assert(commented_entry.license == "SPDX:Apache-2.0#custom-suffix");
    assert(commented_entry.version == "2.1.0");
    assert(commented_entry.abstract == "Swarm coordination skill");

    // 10. Boundary Case: Frontmatter with '...' delimiter
    std::string markdown_dots =
        "---\n"
        "title: Specification Dots\n"
        "license: SPDX:MIT\n"
        "version: 1.0.0\n"
        "...\n"
        "# Specification Dots\nDelimited by triple dots";
    auto dots_entry = librarian.process_ingest_artifact(
        "/nucleus/specs/dots.md",
        markdown_dots,
        "blake3:dotshash"
    );
    assert(dots_entry.title == "Specification Dots");
    assert(dots_entry.license == "SPDX:MIT");
    assert(dots_entry.version == "1.0.0");

    // 11. Minor: Re-entrant start() call
    librarian.start(&bb);
    librarian.start(&bb);
    librarian.stop();

    std::filesystem::remove_all(db_path);
    std::cout << "[SUCCESS] Librarian artifact policy hooks passed!" << std::endl;
    return 0;
}

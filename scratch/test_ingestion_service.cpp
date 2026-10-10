#undef NDEBUG
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>
#include <filesystem>
#include <algorithm>

#include <agentic_blackboard/ArtifactIngestionService.hpp>
#include <ab/ArtifactIngestionService.hpp>
#include <agentic_blackboard/Blackboard.hpp>

using namespace agentic_blackboard;

int main() {
    std::cout << "[Test] Starting ArtifactIngestionService verification..." << std::endl;

    // =========================================================================
    // Test 1: Full YAML frontmatter with comments and quotes
    // =========================================================================
    {
        std::cout << "  - Test 1: Full frontmatter with comments and quotes" << std::endl;
        ArtifactIngestionService service;

        std::string markdown =
            "---\n"
            "title: \"CPG Swarm Coordination #1\" # primary title with inline comment\n"
            "license: SPDX:Apache-2.0#custom-suffix # permissive license\n"
            "version: '2.1.0' # semantic version\n"
            "abstract: Swarm coordination skill # brief abstract\n"
            "---\n"
            "# Heading\n\nContent body.";

        auto entry = service.ingest(
            "/nucleus/specs/cpg_comments.md",
            markdown,
            "sha256:1122334455667788",
            "user:jason",
            "agent:cpg-architect"
        );

        assert(entry.title == "CPG Swarm Coordination #1");
        assert(entry.license == "SPDX:Apache-2.0#custom-suffix");
        assert(entry.version == "2.1.0");
        assert(entry.abstract == "Swarm coordination skill");
        assert(entry.collection_path == "/nucleus/specs");
        assert(entry.logical_name == "cpg_comments.md");
        assert(entry.pid == "urn:ab:artifact:nucleus/specs/cpg_comments.md");
        assert(entry.mime_type == "text/markdown");
        assert(entry.byte_size == markdown.size());
        assert(!entry.uuid.empty());
        assert(entry.created_at_ms > 0);

        // FAIR score should be 100.0 (30 Findable + 25 Accessible + 20 Interoperable + 25 Reusable)
        double score = service.calculate_fair_score(entry);
        assert(score == 100.0);

        int fair_count = 0;
        for (const auto& avu : entry.avus) {
            if (avu.attribute == "fair:score") {
                fair_count++;
                assert(avu.value == "100");
                assert(avu.units == "points");
            }
        }
        assert(fair_count == 1);
    }

    // =========================================================================
    // Test 2: Markdown without frontmatter with '# Heading' fallback
    // =========================================================================
    {
        std::cout << "  - Test 2: Heading fallback without frontmatter" << std::endl;
        ArtifactIngestionService service;

        std::string markdown = "# Architecture Overview\n\nSystem description without YAML frontmatter.";
        auto entry = service.ingest(
            "/nucleus/docs/arch.md",
            markdown,
            "sha256:archhash"
        );

        assert(entry.title == "Architecture Overview");
        assert(entry.license.empty());
        assert(entry.version.empty());
        assert(entry.abstract.empty());
        assert(entry.collection_path == "/nucleus/docs");
        assert(entry.logical_name == "arch.md");
        assert(entry.pid == "urn:ab:artifact:nucleus/docs/arch.md");
        assert(entry.mime_type == "text/markdown");

        // Score: Findable 30 (pid 15 + title 15) + Accessible 25 (hash 15 + path 10) + Interoperable 10 (mime 10 + version 0) + Reusable 0 = 65.0
        double score = service.calculate_fair_score(entry);
        assert(score == 65.0);

        bool found_fair = false;
        for (const auto& avu : entry.avus) {
            if (avu.attribute == "fair:score") {
                found_fair = true;
                assert(avu.value == "65");
                assert(avu.units == "points");
            }
        }
        assert(found_fair);
    }

    // =========================================================================
    // Test 3: Binary file (data.bin) with zeroes: skips frontmatter, octet-stream
    // =========================================================================
    {
        std::cout << "  - Test 3: Binary file skips frontmatter" << std::endl;
        ArtifactIngestionService service;

        std::string binary_data("\0\1\2\0\0\x42", 6);
        auto entry = service.ingest(
            "/models/data.bin",
            binary_data,
            "sha256:binhash"
        );

        assert(entry.title.empty());
        assert(entry.license.empty());
        assert(entry.version.empty());
        assert(entry.abstract.empty());
        assert(entry.mime_type == "application/octet-stream");
        assert(entry.logical_name == "data.bin");
        assert(entry.collection_path == "/models");
        assert(entry.pid == "urn:ab:artifact:models/data.bin");
        assert(entry.byte_size == 6);

        // Score: Findable 15 (pid 15 + title 0) + Accessible 25 (hash 15 + path 10) + Interoperable 0 (octet-stream 0 + version 0) + Reusable 0 = 40.0
        double score = service.calculate_fair_score(entry);
        assert(score == 40.0);

        bool found_fair = false;
        for (const auto& avu : entry.avus) {
            if (avu.attribute == "fair:score") {
                found_fair = true;
                assert(avu.value == "40");
            }
        }
        assert(found_fair);
    }

    // =========================================================================
    // Test 4: Path normalization edge cases
    // =========================================================================
    {
        std::cout << "  - Test 4: Path normalization edge cases" << std::endl;
        ArtifactIngestionService service;

        // 4a. Empty path
        auto empty_e = service.ingest("", "content", "sha256:empty");
        assert(empty_e.pid.empty());
        assert(empty_e.collection_path.empty());
        assert(empty_e.logical_name.empty());
        assert(service.calculate_fair_score(empty_e) == 15.0); // only hash

        // 4b. Slashes only
        auto slash_e = service.ingest("///", "content", "sha256:slash");
        assert(slash_e.pid.empty());
        assert(slash_e.collection_path.empty());
        assert(slash_e.logical_name.empty());

        // 4c. Traversal with ..
        auto dotdot_e = service.ingest("../../escaped.md", "content", "sha256:dotdot");
        assert(dotdot_e.pid.empty());
        assert(dotdot_e.collection_path.empty());
        assert(dotdot_e.logical_name.empty());

        auto dotdot_only = service.ingest("..", "content", "sha256:dotdot_only");
        assert(dotdot_only.pid.empty());

        auto dot_only = service.ingest(".", "content", "sha256:dot_only");
        assert(dot_only.pid.empty());

        // 4d. Redundant slashes in valid path
        auto norm_e = service.ingest("//nested///subfolder//file.txt", "hello", "sha256:nested");
        assert(norm_e.collection_path == "/nested/subfolder");
        assert(norm_e.logical_name == "file.txt");
        assert(norm_e.pid == "urn:ab:artifact:nested/subfolder/file.txt");
    }

    // =========================================================================
    // Test 5: Custom filter injection
    // =========================================================================
    {
        std::cout << "  - Test 5: Custom filter injection" << std::endl;
        ArtifactIngestionService service;

        class WatermarkFilter : public IIngestionFilter {
        public:
            void filter(IngestionContext& ctx) override {
                ctx.avus.push_back(AVUTriple{"provenance:pipeline", "v2-custom-pipeline", ""});
                if (ctx.title.empty()) {
                    ctx.title = "Generated Fallback Title";
                }
            }
        };

        service.add_filter(std::make_unique<WatermarkFilter>());

        auto entry = service.ingest(
            "/docs/readme.txt",
            "This is unformatted plain text.",
            "sha256:customhash"
        );

        assert(entry.title == "Generated Fallback Title");
        bool has_provenance = false;
        for (const auto& a : entry.avus) {
            if (a.attribute == "provenance:pipeline" && a.value == "v2-custom-pipeline") {
                has_provenance = true;
            }
        }
        assert(has_provenance);
    }

    // =========================================================================
    // Test 6: Custom scoring strategy injection via set_score_strategy
    // =========================================================================
    {
        std::cout << "  - Test 6: Custom scoring strategy injection" << std::endl;
        ArtifactIngestionService service;

        class LenientCustomStrategy : public IFairScoreStrategy {
        public:
            double calculate_score(const ArtifactEntry&) const override {
                return 88.0;
            }
        };

        service.set_score_strategy(std::make_shared<LenientCustomStrategy>());

        auto entry = service.ingest(
            "/docs/test.md",
            "# Test Document",
            "sha256:testhash"
        );

        assert(service.calculate_fair_score(entry) == 88.0);

        bool found_88 = false;
        for (const auto& a : entry.avus) {
            if (a.attribute == "fair:score") {
                found_88 = true;
                assert(a.value == "88");
            }
        }
        assert(found_88);
    }

    // =========================================================================
    // Test 7: Initial AVUs preservation and fair:score deduplication
    // =========================================================================
    {
        std::cout << "  - Test 7: Initial AVUs and fair:score deduplication" << std::endl;
        ArtifactIngestionService service;

        std::vector<AVUTriple> initial = {
            {"fair:score", "0", "fake_score"},
            {"domain", "quantum-computing", ""},
            {"sensitivity", "confidential", "internal"}
        };

        auto entry = service.ingest(
            "/quantum/spec.md",
            "---\ntitle: Quantum Engine\nlicense: MIT\nversion: 1.0\nabstract: Quantum spec\n---\n# Quantum",
            "sha256:qhash",
            "user:alice",
            "agent:quantum-ai",
            initial
        );

        int fair_count = 0;
        bool has_domain = false;
        bool has_sensitivity = false;

        for (const auto& a : entry.avus) {
            if (a.attribute == "fair:score") {
                fair_count++;
                assert(a.value == "100");
                assert(a.units == "points");
            }
            if (a.attribute == "domain" && a.value == "quantum-computing") {
                has_domain = true;
            }
            if (a.attribute == "sensitivity" && a.value == "confidential" && a.units == "internal") {
                has_sensitivity = true;
            }
        }

        assert(fair_count == 1);
        assert(has_domain);
        assert(has_sensitivity);
    }

    // =========================================================================
    // Test 8: Frontmatter with '...' delimiter and description fallback
    // =========================================================================
    {
        std::cout << "  - Test 8: Triple dots delimiter and description fallback" << std::endl;
        ArtifactIngestionService service;

        std::string markdown =
            "---\n"
            "description: Abstract derived from description tag\n"
            "license: Apache-2.0\n"
            "version: 3.0.0\n"
            "...\n"
            "# Heading Title Fallback\n\nBody content.";

        auto entry = service.ingest(
            "/specs/dots.md",
            markdown,
            "sha256:dotshash"
        );

        assert(entry.title == "Heading Title Fallback");
        assert(entry.abstract == "Abstract derived from description tag");
        assert(entry.license == "Apache-2.0");
        assert(entry.version == "3.0.0");
    }

    // =========================================================================
    // Test 9: Blackboard commit integration
    // =========================================================================
    {
        std::cout << "  - Test 9: Blackboard commit integration" << std::endl;
        std::string db_path = "/tmp/test_ingest_service_bb_" + std::to_string(time(nullptr));
        std::filesystem::create_directories(db_path);

        {
            blackboard::Blackboard bb(db_path + "/bb", 1);
            ArtifactIngestionService service(&bb);

            std::string content =
                "---\n"
                "title: Live Ingested Artifact\n"
                "license: MIT\n"
                "version: 1.0.0\n"
                "abstract: Ingestion through pipeline into live Blackboard\n"
                "---\n"
                "# Live Ingested Artifact";

            auto entry = service.ingest(
                "/pipeline/live.md",
                content,
                "sha256:liveartifacthash",
                "user:integration",
                "agent:pipeline-agent"
            );

            // Verify entry was committed to blackboard
            auto retrieved = bb.get_artifact(entry.uuid);
            assert(retrieved.has_value());
            assert(retrieved->uuid == entry.uuid);
            assert(retrieved->pid == "urn:ab:artifact:pipeline/live.md");
            assert(retrieved->title == "Live Ingested Artifact");
            assert(retrieved->content_hash == "sha256:liveartifacthash");

            // Query by fair:score AVU
            auto results = bb.query_by_avu("fair:score", "100");
            assert(!results.empty());
            assert(std::find(results.begin(), results.end(), entry.uuid) != results.end());
        }

        std::filesystem::remove_all(db_path);
    }

    // =========================================================================
    // Test 10: Namespace forwarding in ab namespace
    // =========================================================================
    {
        std::cout << "  - Test 10: Namespace forwarding in ab::" << std::endl;
        ab::ArtifactIngestionService ab_svc;
        auto entry = ab_svc.ingest("/test/forwarding.md", "# Forwarding Test", "sha256:fwd");
        assert(entry.title == "Forwarding Test");
        assert(ab_svc.calculate_fair_score(entry) == 65.0);
    }

    // =========================================================================
    // Test 11: Python script comment is not treated as title
    // =========================================================================
    {
        std::cout << "  - Test 11: Python script comment is not treated as title" << std::endl;
        ArtifactIngestionService service;

        std::string py_code = "# TODO: implement neural coordinator\ndef run():\n    pass\n";
        auto entry = service.ingest(
            "/scripts/coordinator.py",
            py_code,
            "sha256:pyhash"
        );
        assert(entry.title.empty());
        assert(entry.logical_name == "coordinator.py");
        assert(entry.collection_path == "/scripts");

        // Shell script test as well
        std::string sh_code = "#!/bin/bash\n# Deploy cluster\n./deploy\n";
        auto sh_entry = service.ingest(
            "/scripts/deploy.sh",
            sh_code,
            "sha256:shhash"
        );
        assert(sh_entry.title.empty());
        assert(sh_entry.logical_name == "deploy.sh");
        assert(sh_entry.collection_path == "/scripts");

        // C++ source test
        std::string cpp_code = "// Main application\nint main() { return 0; }\n";
        auto cpp_entry = service.ingest(
            "/src/main.cpp",
            cpp_code,
            "sha256:cpphash"
        );
        assert(cpp_entry.title.empty());
        assert(cpp_entry.logical_name == "main.cpp");
    }

    // =========================================================================
    // Test 12: Trailing slash path normalization
    // =========================================================================
    {
        std::cout << "  - Test 12: Trailing slash path does not produce '.' logical name" << std::endl;
        ArtifactIngestionService service;

        auto entry = service.ingest(
            "/nucleus/specs/",
            "# Specs Overview\n\nBody",
            "sha256:specshash"
        );
        assert(entry.logical_name == "specs");
        assert(entry.logical_name != ".");
        assert(entry.collection_path == "/nucleus");
        assert(entry.pid == "urn:ab:artifact:nucleus/specs");
        // Not a markdown extension, so heading is not treated as title
        assert(entry.title.empty());

        // With markdown extension and trailing slash
        auto entry_md = service.ingest(
            "/nucleus/specs.md/",
            "# Specs Overview\n\nBody",
            "sha256:specsmdhash"
        );
        assert(entry_md.logical_name == "specs.md");
        assert(entry_md.logical_name != ".");
        assert(entry_md.collection_path == "/nucleus");
        assert(entry_md.pid == "urn:ab:artifact:nucleus/specs.md");
        assert(entry_md.title == "Specs Overview");

        // Trailing backslash
        auto entry_win = service.ingest(
            "/nucleus/specs\\",
            "# Specs Overview Win\n\nBody",
            "sha256:specswinhash"
        );
        assert(entry_win.logical_name == "specs");
        assert(entry_win.logical_name != ".");
        assert(entry_win.collection_path == "/nucleus");
        assert(entry_win.pid == "urn:ab:artifact:nucleus/specs");

        // Path ending in dot (e.g. /nucleus/specs/.) represents a directory (filename is .),
        // so it must NOT produce logical_name = "." (should be empty)
        auto dot_entry = service.ingest(
            "/nucleus/specs/.",
            "# Specs Dot",
            "sha256:specsdothash"
        );
        assert(dot_entry.logical_name.empty());
        assert(dot_entry.logical_name != ".");
        assert(dot_entry.pid.empty());
    }

    // =========================================================================
    // Test 13: Custom pipeline without FairScoringFilter retains initial AVUs
    // =========================================================================
    {
        std::cout << "  - Test 13: Pipeline without FairScoringFilter retains initial AVUs" << std::endl;
        ArtifactIngestionService service;
        service.clear_filters();
        service.add_filter(std::make_unique<PathNormalizationFilter>());
        service.add_filter(std::make_unique<FrontmatterExtractionFilter>());

        std::vector<AVUTriple> initial = {
            {"custom:tag", "value123", "units"},
            {"system:env", "production", ""}
        };

        auto entry = service.ingest(
            "/configs/settings.json",
            "{\"key\": \"value\"}",
            "sha256:confighash",
            "user1",
            "agent1",
            initial
        );

        assert(entry.avus.size() == 2);
        assert(entry.avus[0].attribute == "custom:tag");
        assert(entry.avus[0].value == "value123");
        assert(entry.avus[0].units == "units");
        assert(entry.avus[1].attribute == "system:env");
        assert(entry.avus[1].value == "production");

        for (const auto& a : entry.avus) {
            assert(a.attribute != "fair:score");
        }
    }

    // =========================================================================
    // Test 14: Aborted flag halts subsequent filters and insert_filter works
    // =========================================================================
    {
        std::cout << "  - Test 14: Aborted flag halts subsequent filters and insert_filter works" << std::endl;
        ArtifactIngestionService service;
        assert(service.filters().size() == 3);

        class AbortFilter : public IIngestionFilter {
        public:
            void filter(IngestionContext& ctx) override {
                ctx.aborted = true;
                ctx.error_message = "Rejected by validation policy";
            }
        };

        class SentinelFilter : public IIngestionFilter {
        public:
            void filter(IngestionContext& ctx) override {
                ctx.title = "SHOULD_NEVER_RUN";
            }
        };

        // Insert AbortFilter at index 1 (after PathNormalization, before FrontmatterExtraction)
        service.insert_filter(1, std::make_unique<AbortFilter>());
        assert(service.filters().size() == 4);
        // Insert SentinelFilter at index 2
        service.insert_filter(2, std::make_unique<SentinelFilter>());
        assert(service.filters().size() == 5);

        auto entry = service.ingest(
            "/docs/aborted.md",
            "# Heading That Should Be Ignored",
            "sha256:aborthash"
        );

        // Path was normalized by filter 0
        assert(entry.logical_name == "aborted.md");
        // Sentinel filter and Frontmatter filter skipped
        assert(entry.title.empty());
        assert(entry.title != "SHOULD_NEVER_RUN");
        // FairScoringFilter skipped
        for (const auto& a : entry.avus) {
            assert(a.attribute != "fair:score");
        }
    }

    // =========================================================================
    // Test 15: Traversal path and aborted pipeline do not commit to Blackboard
    // =========================================================================
    {
        std::cout << "  - Test 15: Traversal path and aborted pipeline do not commit to Blackboard" << std::endl;
        std::string db_path = "/tmp/test_ingest_guard_bb_" + std::to_string(time(nullptr));
        std::filesystem::create_directories(db_path);

        {
            blackboard::Blackboard bb(db_path + "/bb", 1);
            ArtifactIngestionService service(&bb);

            // Traversal path
            auto entry_traversal = service.ingest(
                "../../etc/shadow",
                "secret data",
                "sha256:shadowhash",
                "user:attacker",
                "agent:evil"
            );
            assert(entry_traversal.logical_name.empty());
            assert(entry_traversal.pid.empty());
            assert(!bb.get_artifact(entry_traversal.uuid).has_value());

            // Aborted pipeline does not commit
            class AbortingFilter : public IIngestionFilter {
            public:
                void filter(IngestionContext& ctx) override {
                    ctx.aborted = true;
                    ctx.error_message = "Ingestion rejected";
                }
            };

            ArtifactIngestionService service_abort(&bb);
            service_abort.insert_filter(0, std::make_unique<AbortingFilter>());

            auto entry_aborted = service_abort.ingest(
                "/valid/path.md",
                "# Valid Heading",
                "sha256:validhash",
                "user:normal",
                "agent:normal"
            );
            assert(!bb.get_artifact(entry_aborted.uuid).has_value());
        }

        std::filesystem::remove_all(db_path);
    }

    std::cout << "[SUCCESS] ArtifactIngestionService verification passed!" << std::endl;
    return 0;
}

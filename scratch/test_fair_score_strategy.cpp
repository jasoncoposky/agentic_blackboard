#undef NDEBUG
#include <cassert>
#include <iostream>
#include <memory>
#include <ab/FairScoreStrategy.hpp>
#include <agentic_blackboard/schema.hpp>

int main() {
    std::cout << "[Test] Starting FairScoreStrategy verification..." << std::endl;

    agentic_blackboard::StandardFairScoreStrategy strategy;

    // Test 1: Full compliance artifact earns 100.0 points
    agentic_blackboard::ArtifactEntry full;
    full.pid = "urn:ab:artifact:docs/spec.md";
    full.collection_path = "/docs";
    full.logical_name = "spec.md";
    full.title = "FAIR Specification";
    full.primary_locator = "vault/ab/cd/hash";
    full.content_hash = "sha256:123456";
    full.mime_type = "text/markdown";
    full.version = "1.0.0";
    full.license = "SPDX:Apache-2.0";
    full.abstract = "Comprehensive FAIR compliance specification.";

    double score = strategy.calculate_score(full);
    assert(score == 100.0);

    // Test 2: Minimal artifact with only hash earns 15.0 points (Accessible)
    agentic_blackboard::ArtifactEntry minimal;
    minimal.content_hash = "sha256:minimal";
    score = strategy.calculate_score(minimal);
    assert(score == 15.0);

    // Test 3: Slashes-only or default-prefix PID gets 0 PID points, only hash earns 15.0 points
    agentic_blackboard::ArtifactEntry bad_pid;
    bad_pid.pid = "urn:ab:artifact:";
    bad_pid.content_hash = "sha256:hash";
    assert(strategy.calculate_score(bad_pid) == 15.0);

    // Test 4: Custom Strategy can be plugged in (polymorphism test)
    class CustomLenientStrategy : public agentic_blackboard::IFairScoreStrategy {
    public:
        double calculate_score(const agentic_blackboard::ArtifactEntry& e) const override {
            return e.content_hash.empty() ? 0.0 : 100.0;
        }
    };

    CustomLenientStrategy lenient;
    assert(lenient.calculate_score(minimal) == 100.0);

    // Test 5: Verify forwarder / alias in namespace ab
    ab::StandardFairScoreStrategy alias_strat;
    assert(alias_strat.calculate_score(full) == 100.0);

    // Test 6: Empty artifact earns 0.0 points
    agentic_blackboard::ArtifactEntry empty_entry;
    assert(strategy.calculate_score(empty_entry) == 0.0);

    // Test 7: Granular breakdown test for each rubric item
    // Findable breakdown (max 30 pts)
    agentic_blackboard::ArtifactEntry pid_only;
    pid_only.pid = "urn:ab:artifact:models/weights.bin";
    assert(strategy.calculate_score(pid_only) == 15.0);

    agentic_blackboard::ArtifactEntry title_only;
    title_only.title = "Model Weights";
    assert(strategy.calculate_score(title_only) == 15.0);

    // Accessible breakdown (max 25 pts)
    agentic_blackboard::ArtifactEntry loc_only;
    loc_only.primary_locator = "posix:///data/weights.bin";
    assert(strategy.calculate_score(loc_only) == 15.0);

    agentic_blackboard::ArtifactEntry hash_only;
    hash_only.content_hash = "sha256:abc";
    assert(strategy.calculate_score(hash_only) == 15.0);

    // If both locator and hash are present, still 15.0 (content_hash || primary_locator)
    agentic_blackboard::ArtifactEntry loc_and_hash;
    loc_and_hash.primary_locator = "posix:///data/weights.bin";
    loc_and_hash.content_hash = "sha256:abc";
    assert(strategy.calculate_score(loc_and_hash) == 15.0);

    agentic_blackboard::ArtifactEntry coll_only;
    coll_only.collection_path = "/models";
    assert(strategy.calculate_score(coll_only) == 0.0); // requires both coll and name

    agentic_blackboard::ArtifactEntry name_only;
    name_only.logical_name = "weights.bin";
    assert(strategy.calculate_score(name_only) == 0.0); // requires both coll and name

    agentic_blackboard::ArtifactEntry coll_and_name;
    coll_and_name.collection_path = "/models";
    coll_and_name.logical_name = "weights.bin";
    assert(strategy.calculate_score(coll_and_name) == 10.0);

    // Interoperable breakdown (max 20 pts)
    agentic_blackboard::ArtifactEntry octet_stream;
    octet_stream.mime_type = "application/octet-stream";
    assert(strategy.calculate_score(octet_stream) == 0.0); // octet-stream gives 0

    agentic_blackboard::ArtifactEntry specific_mime;
    specific_mime.mime_type = "application/json";
    assert(strategy.calculate_score(specific_mime) == 10.0);

    agentic_blackboard::ArtifactEntry ver_only;
    ver_only.version = "2.1.0";
    assert(strategy.calculate_score(ver_only) == 10.0);

    // Reusable breakdown (max 25 pts)
    agentic_blackboard::ArtifactEntry lic_only;
    lic_only.license = "SPDX:MIT";
    assert(strategy.calculate_score(lic_only) == 20.0);

    agentic_blackboard::ArtifactEntry abs_only;
    abs_only.abstract = "Pre-trained transformer weights";
    assert(strategy.calculate_score(abs_only) == 5.0);

    // Test 8: Polymorphism via std::unique_ptr<IFairScoreStrategy>
    std::unique_ptr<agentic_blackboard::IFairScoreStrategy> poly_strat =
        std::make_unique<agentic_blackboard::StandardFairScoreStrategy>();
    assert(poly_strat->calculate_score(full) == 100.0);
    assert(poly_strat->calculate_score(empty_entry) == 0.0);

    // Test 9: Construct full artifact using ArtifactBuilder from Task 1 and verify 100.0 score
    auto builder_full = agentic_blackboard::ArtifactBuilder()
        .with_pid("urn:ab:artifact:dataset/sample.parquet")
        .with_path("/dataset", "sample.parquet")
        .with_title("Sample Parquet Dataset")
        .with_primary_locator("vault/cd/ef/hash")
        .with_content_hash("blake3:feedface")
        .with_mime_type("application/vnd.apache.parquet")
        .with_version("1.2.3")
        .with_license("SPDX:Apache-2.0")
        .with_abstract("A complete sample parquet dataset artifact.")
        .build();

    assert(strategy.calculate_score(builder_full) == 100.0);
    assert(poly_strat->calculate_score(builder_full) == 100.0);

    // Test 10: ArtifactBuilder with logical path convenience method
    auto builder_logical = agentic_blackboard::ArtifactBuilder()
        .with_pid("urn:ab:artifact:docs/readme.txt")
        .with_logical_path("/docs/readme.txt")
        .with_title("Readme Documentation")
        .with_content_hash("blake3:readme123")
        .with_mime_type("text/plain")
        .with_version("0.1.0")
        .with_license("MIT")
        .with_abstract("Basic instructions")
        .build();

    assert(strategy.calculate_score(builder_logical) == 100.0);

    std::cout << "[SUCCESS] FairScoreStrategy verification passed!" << std::endl;
    return 0;
}

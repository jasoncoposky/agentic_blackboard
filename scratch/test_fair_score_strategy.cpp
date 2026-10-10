#undef NDEBUG
#include <cassert>
#include <iostream>
#include <memory>
#include <agentic_blackboard/schema.hpp>
#include <agentic_blackboard/FairScoreStrategy.hpp>

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

    // Test 2: Minimal artifact with only hash
    agentic_blackboard::ArtifactEntry minimal;
    minimal.content_hash = "sha256:minimal";
    score = strategy.calculate_score(minimal);
    assert(score == 10.0);

    // Test 3: Slashes-only or empty PID gets 0 PID points
    agentic_blackboard::ArtifactEntry bad_pid;
    bad_pid.pid = "urn:ab:artifact:";
    bad_pid.content_hash = "sha256:hash";
    assert(strategy.calculate_score(bad_pid) == 10.0);

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
    // Findable breakdown (35 pts total)
    agentic_blackboard::ArtifactEntry pid_only;
    pid_only.pid = "urn:ab:artifact:models/weights.bin";
    assert(strategy.calculate_score(pid_only) == 15.0);

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

    agentic_blackboard::ArtifactEntry title_only;
    title_only.title = "Model Weights";
    assert(strategy.calculate_score(title_only) == 10.0);

    // Accessible breakdown (25 pts total)
    agentic_blackboard::ArtifactEntry loc_only;
    loc_only.primary_locator = "posix:///data/weights.bin";
    assert(strategy.calculate_score(loc_only) == 15.0);

    agentic_blackboard::ArtifactEntry hash_only;
    hash_only.content_hash = "sha256:abc";
    assert(strategy.calculate_score(hash_only) == 10.0);

    // Interoperable breakdown (20 pts total)
    agentic_blackboard::ArtifactEntry octet_stream;
    octet_stream.mime_type = "application/octet-stream";
    assert(strategy.calculate_score(octet_stream) == 0.0); // octet-stream gives 0

    agentic_blackboard::ArtifactEntry specific_mime;
    specific_mime.mime_type = "application/json";
    assert(strategy.calculate_score(specific_mime) == 10.0);

    agentic_blackboard::ArtifactEntry ver_only;
    ver_only.version = "2.1.0";
    assert(strategy.calculate_score(ver_only) == 10.0);

    // Reusable breakdown (20 pts total)
    agentic_blackboard::ArtifactEntry lic_only;
    lic_only.license = "MIT";
    assert(strategy.calculate_score(lic_only) == 15.0);

    agentic_blackboard::ArtifactEntry abs_only;
    abs_only.abstract = "Pre-trained transformer weights";
    assert(strategy.calculate_score(abs_only) == 5.0);

    // Test 8: Polymorphism via std::unique_ptr<IFairScoreStrategy>
    std::unique_ptr<agentic_blackboard::IFairScoreStrategy> poly_strat =
        std::make_unique<agentic_blackboard::StandardFairScoreStrategy>();
    assert(poly_strat->calculate_score(full) == 100.0);
    assert(poly_strat->calculate_score(empty_entry) == 0.0);

    std::cout << "[SUCCESS] FairScoreStrategy verification passed!" << std::endl;
    return 0;
}

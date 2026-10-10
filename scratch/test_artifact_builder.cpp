#undef NDEBUG
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <unordered_set>
#include <agentic_blackboard/schema.hpp>

int main() {
    std::cout << "[Test] Starting ArtifactBuilder verification..." << std::endl;

    // Test 1: Fluent construction with automatic UUID and timestamp minting
    auto entry1 = agentic_blackboard::ArtifactBuilder()
        .with_path("/nucleus/specs", "storage.md")
        .with_content_hash("sha256:abcd1234")
        .with_mime_type("text/markdown")
        .with_byte_size(2048)
        .with_title("Storage Spec")
        .with_abstract("Design doc for storage")
        .with_license("SPDX:Apache-2.0")
        .with_version("1.0.0")
        .with_primary_locator("posix:///var/data/storage.md")
        .add_avu("curation:status", "approved", "")
        .add_avu(agentic_blackboard::AVUTriple{"system:owner", "admin", ""})
        .add_derived_from("art-parent-001")
        .build();

    assert(!entry1.uuid.empty());
    assert(entry1.pid == "urn:ab:artifact:nucleus/specs/storage.md");
    assert(entry1.collection_path == "/nucleus/specs");
    assert(entry1.logical_name == "storage.md");
    assert(entry1.content_hash == "sha256:abcd1234");
    assert(entry1.mime_type == "text/markdown");
    assert(entry1.byte_size == 2048);
    assert(entry1.title == "Storage Spec");
    assert(entry1.abstract == "Design doc for storage");
    assert(entry1.license == "SPDX:Apache-2.0");
    assert(entry1.version == "1.0.0");
    assert(entry1.primary_locator == "posix:///var/data/storage.md");
    assert(entry1.created_at_ms > 0);
    assert(entry1.avus.size() == 2);
    assert(entry1.avus[0].attribute == "curation:status");
    assert(entry1.avus[1].attribute == "system:owner");
    assert(entry1.derived_from_uuids.size() == 1);
    assert(entry1.derived_from_uuids[0] == "art-parent-001");

    // Test 2: Explicit UUID and PID preservation
    auto entry2 = agentic_blackboard::ArtifactBuilder()
        .with_uuid("art-custom-001")
        .with_pid("urn:custom:pid")
        .with_path("/archive", "data.bin")
        .with_created_at_ms(1700000000000ULL)
        .build();

    assert(entry2.uuid == "art-custom-001");
    assert(entry2.pid == "urn:custom:pid");
    assert(entry2.collection_path == "/archive");
    assert(entry2.logical_name == "data.bin");
    assert(entry2.created_at_ms == 1700000000000ULL);

    // Test 3: Empty path leaves PID empty
    auto entry3 = agentic_blackboard::ArtifactBuilder()
        .with_content_hash("sha256:onlyhash")
        .build();

    assert(entry3.pid.empty());
    assert(entry3.collection_path.empty());
    assert(entry3.logical_name.empty());

    // Test 4: Slashes-only path ("/", "/") and ("///", "///") leaves PID empty
    auto entry_slash1 = agentic_blackboard::ArtifactBuilder()
        .with_path("/", "/")
        .build();
    assert(entry_slash1.pid.empty());

    auto entry_slash2 = agentic_blackboard::ArtifactBuilder()
        .with_path("///", "///")
        .build();
    assert(entry_slash2.pid.empty());

    // Test 5: Root collection ("", "file.txt") generates correct URN
    auto entry_root = agentic_blackboard::ArtifactBuilder()
        .with_path("", "file.txt")
        .build();
    assert(entry_root.pid == "urn:ab:artifact:file.txt");

    // Test 6: Trailing slashes in logical name ("dir", "file.txt/") trimmed cleanly
    auto entry_trailing = agentic_blackboard::ArtifactBuilder()
        .with_path("dir", "file.txt/")
        .build();
    assert(entry_trailing.pid == "urn:ab:artifact:dir/file.txt");

    // Test 7: with_logical_path splits into collection path and logical name
    auto entry_log_path = agentic_blackboard::ArtifactBuilder()
        .with_logical_path("/nucleus/specs/doc.md")
        .build();
    assert(entry_log_path.collection_path == "/nucleus/specs");
    assert(entry_log_path.logical_name == "doc.md");
    assert(entry_log_path.pid == "urn:ab:artifact:nucleus/specs/doc.md");

    // Test 8: AVUTriple&& rvalue move overload
    agentic_blackboard::AVUTriple avu_moved{"science:metric", "42", "joules"};
    auto entry_avu = agentic_blackboard::ArtifactBuilder()
        .add_avu(std::move(avu_moved))
        .build();
    assert(entry_avu.avus.size() == 1);
    assert(entry_avu.avus[0].attribute == "science:metric");
    assert(entry_avu.avus[0].value == "42");
    assert(entry_avu.avus[0].units == "joules");

    // Test 9: Multi-threaded concurrency test (8 threads x 100 artifacts = 800 unique UUIDs)
    constexpr int num_threads = 8;
    constexpr int artifacts_per_thread = 100;
    std::vector<std::thread> threads;
    std::vector<std::string> uuids(num_threads * artifacts_per_thread);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([t, &uuids]() {
            for (int i = 0; i < artifacts_per_thread; ++i) {
                auto entry = agentic_blackboard::ArtifactBuilder()
                    .with_path("/data", "file" + std::to_string(i))
                    .build();
                uuids[t * artifacts_per_thread + i] = std::move(entry.uuid);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    std::unordered_set<std::string> unique_uuids(uuids.begin(), uuids.end());
    assert(unique_uuids.size() == static_cast<size_t>(num_threads * artifacts_per_thread));

    std::cout << "[SUCCESS] ArtifactBuilder verification passed!" << std::endl;
    return 0;
}

#include "agentic_blackboard/StorageManager.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

void test_basic_and_fallback_routing() {
    std::string test_dir = "/tmp/test_storage_mgr_" + std::to_string(time(nullptr));
    std::filesystem::create_directories(test_dir);

    // Test namespace aliases
    static_assert(std::is_same_v<agentic_blackboard::storage::StorageManager, ab::storage::StorageManager>);
    static_assert(std::is_same_v<agentic_blackboard::storage::StorageManager, blackboard::storage::StorageManager>);

    blackboard::storage::StorageManager mgr(test_dir);

    // 1. Verify default POSIX driver is registered
    assert(mgr.has_driver("default_posix_cas"));
    assert(!mgr.has_driver("non_existent_driver"));
    assert(mgr.get_driver("") != nullptr);
    assert(mgr.get_driver("default_posix_cas") != nullptr);
    assert(mgr.get_driver("non_existent_driver") == nullptr);
    assert(mgr.default_driver_id() == "default_posix_cas");

    // 2. Store stream through manager with explicit driver
    std::string payload = "# System Architecture Spec\nVersion 1.0";
    std::istringstream stream_in(payload);
    auto res = mgr.store("default_posix_cas", stream_in).get();

    assert(!res.digest.empty());
    assert(res.bytes_written == payload.size());
    assert(res.driver_id == "default_posix_cas");

    // 3. Fetch stream through manager with explicit driver
    auto fetched_stream = mgr.retrieve("default_posix_cas", res.locator);
    assert(fetched_stream != nullptr);
    std::string body((std::istreambuf_iterator<char>(*fetched_stream)), std::istreambuf_iterator<char>());
    assert(body == payload);

    // 4. Verify stream through manager with explicit driver
    assert(mgr.verify("default_posix_cas", res.locator, res.digest));
    assert(!mgr.verify("default_posix_cas", res.locator, "sha256:0000000000000000000000000000000000000000000000000000000000000000"));

    // 5. Fallback routing test: store without driver_id (falls back to default driver)
    std::string payload2 = "Secondary Spec Payload for Fallback Routing";
    std::istringstream stream_in2(payload2);
    auto res2 = mgr.store(stream_in2).get();
    assert(!res2.digest.empty());
    assert(res2.bytes_written == payload2.size());

    // Fallback routing: retrieve without driver_id or with empty driver_id
    auto fetched2 = mgr.retrieve(res2.locator);
    assert(fetched2 != nullptr);
    std::string body2((std::istreambuf_iterator<char>(*fetched2)), std::istreambuf_iterator<char>());
    assert(body2 == payload2);

    auto fetched2_empty = mgr.retrieve("", res2.locator);
    assert(fetched2_empty != nullptr);
    std::string body2_empty((std::istreambuf_iterator<char>(*fetched2_empty)), std::istreambuf_iterator<char>());
    assert(body2_empty == payload2);

    // Verify via fallback routing
    assert(mgr.verify(res2.locator, res2.digest));
    assert(mgr.verify("", res2.locator, res2.digest));

    // Range retrieval through StorageManager
    blackboard::storage::ByteRange range{0, 9}; // "Secondary"
    auto range_stream = mgr.retrieve("", res2.locator, range);
    assert(range_stream != nullptr);
    std::string range_body((std::istreambuf_iterator<char>(*range_stream)), std::istreambuf_iterator<char>());
    assert(range_body == "Secondary");

    // Unlink through manager
    assert(mgr.unlink("default_posix_cas", res2.locator));
    assert(mgr.retrieve("default_posix_cas", res2.locator) == nullptr);

    std::filesystem::remove_all(test_dir);
    std::cout << "[PASS] Basic and fallback routing tests passed." << std::endl;
}

void test_multi_driver_and_cross_fallback() {
    std::string test_dir1 = "/tmp/test_storage_mgr1_" + std::to_string(time(nullptr));
    std::string test_dir2 = "/tmp/test_storage_mgr2_" + std::to_string(time(nullptr));
    std::filesystem::create_directories(test_dir1);
    std::filesystem::create_directories(test_dir2);

    blackboard::storage::StorageManager mgr(test_dir1);

    // Register second driver
    auto driver2 = std::make_shared<blackboard::storage::PosixCasDriver>(test_dir2, "posix_secondary");
    mgr.register_driver("posix_secondary", driver2);

    assert(mgr.has_driver("posix_secondary"));
    assert(mgr.get_driver("posix_secondary") == driver2);

    // Store in secondary driver specifically
    std::string content = "Content stored in secondary driver";
    std::istringstream in_stream(content);
    auto res = mgr.store("posix_secondary", in_stream).get();
    assert(res.driver_id == "posix_secondary");

    // Primary driver should NOT have it
    auto primary_fetch = mgr.retrieve("default_posix_cas", res.locator);
    assert(primary_fetch == nullptr);

    // Fallback routing (empty driver_id or overload) should find it in secondary driver!
    auto fallback_fetch = mgr.retrieve("", res.locator);
    assert(fallback_fetch != nullptr);
    std::string read_content((std::istreambuf_iterator<char>(*fallback_fetch)), std::istreambuf_iterator<char>());
    assert(read_content == content);

    // Fallback verify should also succeed
    assert(mgr.verify("", res.locator, res.digest));

    // Telemetry aggregation stat_all()
    auto stats = mgr.stat_all();
    assert(stats.size() == 2);
    assert(stats.find("default_posix_cas") != stats.end());
    assert(stats.find("posix_secondary") != stats.end());
    assert(stats["posix_secondary"].total_capacity_bytes > 0);

    // Fallback unlink
    assert(mgr.unlink("", res.locator));
    assert(mgr.retrieve("", res.locator) == nullptr);

    // Error handling: register with empty name or null driver
    bool caught_invalid = false;
    try {
        mgr.register_driver("", driver2);
    } catch (const std::invalid_argument&) {
        caught_invalid = true;
    }
    assert(caught_invalid);

    caught_invalid = false;
    try {
        mgr.register_driver("null_driver", nullptr);
    } catch (const std::invalid_argument&) {
        caught_invalid = true;
    }
    assert(caught_invalid);

    // Error handling: store/retrieve with non-existent driver
    bool caught_not_found = false;
    try {
        std::istringstream dummy("foo");
        mgr.store("unknown_driver", dummy);
    } catch (const std::runtime_error&) {
        caught_not_found = true;
    }
    assert(caught_not_found);

    caught_not_found = false;
    try {
        mgr.retrieve("unknown_driver", res.locator);
    } catch (const std::runtime_error&) {
        caught_not_found = true;
    }
    assert(caught_not_found);

    std::filesystem::remove_all(test_dir1);
    std::filesystem::remove_all(test_dir2);
    std::cout << "[PASS] Multi-driver and cross-driver fallback tests passed." << std::endl;
}

void test_concurrent_access() {
    std::string test_dir = "/tmp/test_storage_mgr_conc_" + std::to_string(time(nullptr));
    std::filesystem::create_directories(test_dir);

    blackboard::storage::StorageManager mgr(test_dir);

    constexpr int kNumThreads = 8;
    constexpr int kIterations = 20;
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&mgr, t]() {
            for (int i = 0; i < kIterations; ++i) {
                std::string msg = "Thread " + std::to_string(t) + " Payload " + std::to_string(i);
                std::istringstream in(msg);
                auto res = mgr.store(in).get();
                assert(!res.digest.empty());

                auto stream = mgr.retrieve(res.locator);
                assert(stream != nullptr);
                std::string fetched((std::istreambuf_iterator<char>(*stream)), std::istreambuf_iterator<char>());
                assert(fetched == msg);

                assert(mgr.verify(res.locator, res.digest));
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto stats = mgr.stat_all();
    assert(stats.find("default_posix_cas") != stats.end());

    std::filesystem::remove_all(test_dir);
    std::cout << "[PASS] Concurrent access tests passed." << std::endl;
}

int main() {
    // Standard test required by plan
    std::string test_dir = "/tmp/test_storage_mgr_" + std::to_string(time(nullptr));
    std::filesystem::create_directories(test_dir);

    blackboard::storage::StorageManager mgr(test_dir);

    // Verify default POSIX driver is registered
    assert(mgr.has_driver("default_posix_cas"));

    // Store stream through manager
    std::string payload = "# System Architecture Spec\nVersion 1.0";
    std::istringstream stream_in(payload);
    auto res = mgr.store("default_posix_cas", stream_in).get();

    assert(!res.digest.empty());
    assert(res.bytes_written == payload.size());

    // Fetch stream through manager
    auto fetched_stream = mgr.retrieve("default_posix_cas", res.locator);
    assert(fetched_stream != nullptr);
    std::string body((std::istreambuf_iterator<char>(*fetched_stream)), std::istreambuf_iterator<char>());
    assert(body == payload);

    std::filesystem::remove_all(test_dir);

    // Additional comprehensive test suites
    test_basic_and_fallback_routing();
    test_multi_driver_and_cross_fallback();
    test_concurrent_access();

    std::cout << "[SUCCESS] StorageManager routing tests passed!" << std::endl;
    return 0;
}

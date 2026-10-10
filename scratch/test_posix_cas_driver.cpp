#undef NDEBUG
#include "agentic_blackboard/PosixCasDriver.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cctype>

// Helper to count files in directory
static size_t count_files_in_dir(const std::filesystem::path& dir) {
    if (!std::filesystem::exists(dir)) return 0;
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            count++;
        }
    }
    return count;
}


int main() {
    std::string test_vault = "/tmp/test_ab_cas_vault_" + std::to_string(time(nullptr));
    std::filesystem::create_directories(test_vault);

    // 0. Test Namespace Compatibility
    {
        static_assert(std::is_same_v<agentic_blackboard::storage::PosixCasDriver, ab::storage::PosixCasDriver>);
        static_assert(std::is_same_v<agentic_blackboard::storage::PosixCasDriver, blackboard::storage::PosixCasDriver>);
        std::cout << "[PASS] Namespace compatibility verified (agentic_blackboard::storage, ab::storage, blackboard::storage)" << std::endl;
    }

    agentic_blackboard::storage::PosixCasDriver driver(test_vault);

    // 1. Test Put Stream
    std::string content = "Hello FAIR Data Management World!";
    std::istringstream in1(content);
    auto result1 = driver.put_stream(in1).get();

    assert(!result1.digest.empty());
    assert(result1.digest.rfind("sha256:", 0) == 0); // Default EVP is sha256
    assert(result1.bytes_written == content.size());
    std::cout << "[PASS] put_stream produced digest: " << result1.digest << std::endl;

    // 2. Test Deduplication
    std::istringstream in2(content);
    auto result2 = driver.put_stream(in2).get();
    assert(result1.digest == result2.digest);
    std::cout << "[PASS] Deduplication verified for digest: " << result2.digest << std::endl;

    // 2b. Test CAS Pre-check Deduplication (instant return when expected_hash exists)
    {
        std::istringstream in_precheck("");
        auto res_precheck = driver.put_stream_sync(in_precheck, result1.digest);
        assert(res_precheck.digest == result1.digest);
        assert(res_precheck.bytes_written == content.size());
        assert(res_precheck.locator == result1.locator);
        std::cout << "[PASS] CAS Pre-check deduplication verified for digest: " << res_precheck.digest << std::endl;
    }

    // 3. Test Get Stream
    auto stream_out = driver.get_stream(result1.locator);
    assert(stream_out != nullptr);
    std::string retrieved((std::istreambuf_iterator<char>(*stream_out)), std::istreambuf_iterator<char>());
    assert(retrieved == content);
    stream_out.reset();
    std::cout << "[PASS] get_stream accurately retrieved content" << std::endl;

    // 4. Test Byte Range Retrieval
    agentic_blackboard::storage::ByteRange range{6, 4}; // "FAIR"
    auto range_out = driver.get_stream(result1.locator, range);
    assert(range_out != nullptr);
    std::string range_str((std::istreambuf_iterator<char>(*range_out)), std::istreambuf_iterator<char>());
    assert(range_str == "FAIR");
    range_out.reset();
    std::cout << "[PASS] Byte range retrieval verified: " << range_str << std::endl;

    // 5. Test Verify Digest (matching, corrupt)
    assert(driver.verify_digest(result1.locator, result1.digest) == true);
    assert(driver.verify_digest(result1.locator, "sha256:0000000000000000000000000000000000000000000000000000000000000000") == false);
    std::cout << "[PASS] verify_digest verified bit-rot detection" << std::endl;

    // 6. Test Multi-chunk payload (>128 KB, exceeding 64 KB buffer)
    {
        const size_t large_size = 256 * 1024; // 256 KB
        std::string large_payload;
        large_payload.resize(large_size);
        for (size_t i = 0; i < large_size; ++i) {
            large_payload[i] = static_cast<char>('A' + (i % 26));
        }

        std::istringstream large_in(large_payload);
        auto large_result = driver.put_stream(large_in).get();
        assert(large_result.bytes_written == large_size);
        assert(!large_result.digest.empty());

        // Verify retrieval of multi-chunk stream
        auto large_stream = driver.get_stream(large_result.locator);
        assert(large_stream != nullptr);
        std::string large_retrieved((std::istreambuf_iterator<char>(*large_stream)), std::istreambuf_iterator<char>());
        assert(large_retrieved.size() == large_size);
        assert(large_retrieved == large_payload);
        large_stream.reset();

        // Verify byte range crossing 64 KB chunk boundary
        agentic_blackboard::storage::ByteRange chunk_crossing_range{65530, 20};
        auto cross_stream = driver.get_stream(large_result.locator, chunk_crossing_range);
        assert(cross_stream != nullptr);
        std::string cross_str((std::istreambuf_iterator<char>(*cross_stream)), std::istreambuf_iterator<char>());
        assert(cross_str == large_payload.substr(65530, 20));
        cross_stream.reset();

        assert(driver.verify_digest(large_result.locator, large_result.digest) == true);
        std::cout << "[PASS] Multi-chunk payload (>128 KB: 256 KB) put, get, and range retrieval verified" << std::endl;
    }

    // 7. Test Active Stream Accounting and driver.stat()
    {
        auto stats_before = driver.stat();
        assert(stats_before.total_capacity_bytes > 0);
        assert(stats_before.free_capacity_bytes > 0);
        assert(stats_before.active_streams == 0);

        auto s1 = driver.get_stream(result1.locator);
        assert(s1 != nullptr);
        assert(driver.stat().active_streams == 1);

        auto s2 = driver.get_stream(result1.locator, agentic_blackboard::storage::ByteRange{0, 5});
        assert(s2 != nullptr);
        assert(driver.stat().active_streams == 2);

        s1.reset();
        assert(driver.stat().active_streams == 1);

        s2.reset();
        assert(driver.stat().active_streams == 0);

        auto stats_after = driver.stat();
        assert(stats_after.write_throughput_mb_s >= 0.0);
        std::cout << "[PASS] Active stream accounting and driver.stat() verified" << std::endl;
    }

    // 8. Test Expected Hash Mismatch throwing std::runtime_error & Temp File Cleanup
    {
        size_t tmp_files_before = count_files_in_dir(std::filesystem::path(test_vault) / ".tmp");
        std::string mismatch_payload = "Data with wrong expected hash";
        std::istringstream mismatch_in(mismatch_payload);
        std::string bogus_hash = "sha256:0000000000000000000000000000000000000000000000000000000000000000";

        bool threw = false;
        try {
            driver.put_stream_sync(mismatch_in, bogus_hash);
        } catch (const std::runtime_error& e) {
            threw = true;
            std::string msg = e.what();
            assert(msg.find("Digest mismatch") != std::string::npos);
        }
        assert(threw);

        // Verify temp file was cleaned up by RAII guard
        size_t tmp_files_after = count_files_in_dir(std::filesystem::path(test_vault) / ".tmp");
        assert(tmp_files_after == tmp_files_before);
        std::cout << "[PASS] Expected hash mismatch correctly threw std::runtime_error and cleaned up temp file" << std::endl;
    }

    // 9. Test Silent Data Corruption Detection (Input Stream Read Failure) & Cleanup
    {
        size_t tmp_files_before = count_files_in_dir(std::filesystem::path(test_vault) / ".tmp");
        std::istringstream bad_stream("Some valid initial bytes");
        // Force stream into bad state
        bad_stream.setstate(std::ios::badbit);

        bool threw_bad = false;
        try {
            driver.put_stream_sync(bad_stream);
        } catch (const std::runtime_error& e) {
            threw_bad = true;
            std::string msg = e.what();
            assert(msg.find("I/O failure while reading input stream") != std::string::npos);
        }
        assert(threw_bad);

        size_t tmp_files_after = count_files_in_dir(std::filesystem::path(test_vault) / ".tmp");
        assert(tmp_files_after == tmp_files_before);
        std::cout << "[PASS] Input stream I/O failure correctly threw std::runtime_error and cleaned up temp file" << std::endl;
    }

    // 10. Test Path Traversal Protection
    {
        // Absolute traversal
        bool abs_traversal_blocked = false;
        try {
            driver.resolve_path("/etc/passwd");
        } catch (const std::invalid_argument&) {
            abs_traversal_blocked = true;
        }
        assert(abs_traversal_blocked);

        // Relative traversal
        bool rel_traversal_blocked = false;
        try {
            driver.resolve_path("../../../etc/passwd");
        } catch (const std::invalid_argument&) {
            rel_traversal_blocked = true;
        }
        assert(rel_traversal_blocked);

        // get_stream rejects path traversal (either throws or returns nullptr)
        bool get_stream_blocked = false;
        try {
            auto stream = driver.get_stream("../../../etc/passwd");
            if (stream == nullptr) {
                get_stream_blocked = true;
            }
        } catch (const std::invalid_argument&) {
            get_stream_blocked = true;
        }
        assert(get_stream_blocked);

        // unlink rejects path traversal
        assert(driver.unlink("../../../etc/passwd") == false);

        // compute_target_path rejects invalid digest containing traversal
        bool compute_traversal_blocked = false;
        try {
            driver.compute_target_path("../../../etc/passwd");
        } catch (const std::invalid_argument&) {
            compute_traversal_blocked = true;
        }
        assert(compute_traversal_blocked);

        std::cout << "[PASS] Path traversal protection verified (absolute, relative, compute_target_path, unlink)" << std::endl;
    }

    // 11. Test Lowercase/Uppercase Digest Normalization & Prefix Consistency
    {
        // Extract hex part of result1.digest
        std::string raw_digest = result1.digest;
        auto colon = raw_digest.find(':');
        std::string hex_part = (colon != std::string::npos) ? raw_digest.substr(colon + 1) : raw_digest;

        std::string upper_hex = hex_part;
        std::transform(upper_hex.begin(), upper_hex.end(), upper_hex.begin(), ::toupper);

        std::string upper_digest = "SHA256:" + upper_hex;

        // Path computation should match regardless of case
        auto path_lower = driver.compute_target_path(result1.digest);
        auto path_upper = driver.compute_target_path(upper_digest);
        assert(path_lower == path_upper);

        // verify_digest should succeed with uppercase digest
        assert(driver.verify_digest(result1.locator, upper_digest) == true);
        assert(driver.verify_digest(result1.locator, upper_hex) == true);

        // get_stream should find the content using uppercase digest
        auto stream_from_upper = driver.get_stream(upper_digest);
        assert(stream_from_upper != nullptr);
        std::string content_from_upper((std::istreambuf_iterator<char>(*stream_from_upper)), std::istreambuf_iterator<char>());
        assert(content_from_upper == content);

        // Put with uppercase expected_hash succeeds
        std::istringstream in_upper(content);
        auto res_upper = driver.put_stream_sync(in_upper, upper_digest);
        assert(res_upper.digest == result1.digest);

        // Put with explicit blake3: prefix in expected_hash preserves blake3 prefix
        std::string blake3_expected = "blake3:" + hex_part;
        std::istringstream in_blake3(content);
        auto res_blake3 = driver.put_stream_sync(in_blake3, blake3_expected);
        assert(res_blake3.digest == blake3_expected);

        std::cout << "[PASS] Lowercase/uppercase digest normalization and prefix consistency verified" << std::endl;
    }

    // 12. Test driver.unlink(...)
    {
        std::string unlink_content = "Temporary unlinked content";
        std::istringstream in_unlink(unlink_content);
        auto res_unlink = driver.put_stream(in_unlink).get();

        assert(driver.get_stream(res_unlink.locator) != nullptr);

        // First unlink succeeds
        assert(driver.unlink(res_unlink.locator) == true);

        // Subsequent get fails
        assert(driver.get_stream(res_unlink.locator) == nullptr);

        // Second unlink returns false
        assert(driver.unlink(res_unlink.locator) == false);

        // Directory unlink returns false (non-regular file)
        assert(driver.unlink(test_vault + "/.tmp") == false);

        std::cout << "[PASS] driver.unlink(...) verified" << std::endl;
    }

    // Cleanup
    std::filesystem::remove_all(test_vault);
    std::cout << "[SUCCESS] All PosixCasDriver tests passed!" << std::endl;
    return 0;
}

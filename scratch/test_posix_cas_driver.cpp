#include "agentic_blackboard/PosixCasDriver.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <sstream>

int main() {
    std::string test_vault = "/tmp/test_ab_cas_vault_" + std::to_string(time(nullptr));
    std::filesystem::create_directories(test_vault);

    blackboard::storage::PosixCasDriver driver(test_vault);

    // 1. Test Put Stream
    std::string content = "Hello FAIR Data Management World!";
    std::istringstream in1(content);
    auto result1 = driver.put_stream(in1).get();

    assert(!result1.digest.empty());
    assert(result1.bytes_written == content.size());
    std::cout << "[PASS] put_stream produced digest: " << result1.digest << std::endl;

    // 2. Test Deduplication (Second put of identical content returns same digest and does not duplicate)
    std::istringstream in2(content);
    auto result2 = driver.put_stream(in2).get();
    assert(result1.digest == result2.digest);
    std::cout << "[PASS] Deduplication verified for digest: " << result2.digest << std::endl;

    // 3. Test Get Stream
    auto stream_out = driver.get_stream(result1.locator);
    assert(stream_out != nullptr);
    std::string retrieved((std::istreambuf_iterator<char>(*stream_out)), std::istreambuf_iterator<char>());
    assert(retrieved == content);
    std::cout << "[PASS] get_stream accurately retrieved content" << std::endl;

    // 4. Test Byte Range Retrieval
    blackboard::storage::ByteRange range{6, 4}; // "FAIR"
    auto range_out = driver.get_stream(result1.locator, range);
    assert(range_out != nullptr);
    std::string range_str((std::istreambuf_iterator<char>(*range_out)), std::istreambuf_iterator<char>());
    assert(range_str == "FAIR");
    std::cout << "[PASS] Byte range retrieval verified: " << range_str << std::endl;

    // 5. Test Verify Digest
    assert(driver.verify_digest(result1.locator, result1.digest) == true);
    assert(driver.verify_digest(result1.locator, "blake3:corrupt") == false);
    std::cout << "[PASS] verify_digest verified bit-rot detection" << std::endl;

    // Cleanup
    std::filesystem::remove_all(test_vault);
    std::cout << "[SUCCESS] All PosixCasDriver tests passed!" << std::endl;
    return 0;
}

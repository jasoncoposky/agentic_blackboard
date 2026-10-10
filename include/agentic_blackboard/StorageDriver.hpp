#pragma once
#include <string>
#include <string_view>
#include <memory>
#include <istream>
#include <future>
#include <optional>
#include <cstdint>

namespace agentic_blackboard::storage {

struct ByteRange {
    uint64_t offset{0};
    uint64_t length{0};
};

struct PutResult {
    std::string digest;
    uint64_t bytes_written{0};
    std::string driver_id;
    std::string locator;
    uint64_t timestamp_ms{0};
};

struct StorageStats {
    uint64_t total_capacity_bytes{0};
    uint64_t free_capacity_bytes{0};
    uint32_t active_streams{0};
    double write_throughput_mb_s{0.0};
};

class IStorageDriver {
public:
    virtual ~IStorageDriver() = default;
    virtual auto put_stream(std::istream& in, std::string_view expected_hash = {}) 
        -> std::future<PutResult> = 0;
    virtual auto get_stream(std::string_view locator, std::optional<ByteRange> range = {}) 
        -> std::unique_ptr<std::istream> = 0;
    virtual bool verify_digest(std::string_view locator, std::string_view expected_hash) = 0;
    virtual bool unlink(std::string_view locator) = 0;
    virtual StorageStats stat() = 0;
};

} // namespace agentic_blackboard::storage

namespace ab = agentic_blackboard;
namespace blackboard = agentic_blackboard;


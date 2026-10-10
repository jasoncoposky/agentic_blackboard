#pragma once

#include <agentic_blackboard/StorageDriver.hpp>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <memory>
#include <istream>
#include <future>
#include <optional>

namespace agentic_blackboard::storage {

class PosixCasDriver : public IStorageDriver {
public:
    explicit PosixCasDriver(std::filesystem::path vault_root, std::string driver_id = "posix_cas_default");
    ~PosixCasDriver() override = default;

    PosixCasDriver(const PosixCasDriver&) = delete;
    PosixCasDriver& operator=(const PosixCasDriver&) = delete;
    PosixCasDriver(PosixCasDriver&&) noexcept;
    PosixCasDriver& operator=(PosixCasDriver&&) noexcept;

    auto put_stream(std::istream& in, std::string_view expected_hash = {}) 
        -> std::future<PutResult> override;

    auto get_stream(std::string_view locator, std::optional<ByteRange> range = {}) 
        -> std::unique_ptr<std::istream> override;

    bool verify_digest(std::string_view locator, std::string_view expected_hash) override;

    bool unlink(std::string_view locator) override;

    StorageStats stat() override;

    PutResult put_stream_sync(std::istream& in, std::string_view expected_hash = {}) override;

    const std::filesystem::path& vault_root() const noexcept { return vault_root_; }
    const std::string& driver_id() const noexcept { return driver_id_; }

    std::filesystem::path resolve_path(std::string_view locator) const;
    std::filesystem::path compute_target_path(std::string_view digest) const;

private:
    std::filesystem::path vault_root_;
    std::string driver_id_;
    std::shared_ptr<std::atomic<uint32_t>> active_streams_{std::make_shared<std::atomic<uint32_t>>(0)};
    std::atomic<uint64_t> total_bytes_written_{0};
    std::atomic<uint64_t> total_write_time_ns_{0};
};

} // namespace agentic_blackboard::storage

namespace ab = agentic_blackboard;
namespace blackboard = agentic_blackboard;


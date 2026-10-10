#pragma once

#include <agentic_blackboard/StorageDriver.hpp>

#include <string>
#include <string_view>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <future>
#include <optional>
#include <istream>

namespace agentic_blackboard::storage {

class StorageManager {
public:
    explicit StorageManager(const std::string& default_vault_dir = "/var/lib/agentic-blackboard/vault");
    ~StorageManager() = default;

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;
    StorageManager(StorageManager&& other) noexcept;
    StorageManager& operator=(StorageManager&& other) noexcept;

    void register_driver(const std::string& driver_id, std::shared_ptr<IStorageDriver> driver);
    bool has_driver(const std::string& driver_id) const;
    std::shared_ptr<IStorageDriver> get_driver(const std::string& driver_id = "") const;

    std::future<PutResult> store(const std::string& driver_id, std::istream& in, std::string_view expected_hash = {});
    std::future<PutResult> store(std::istream& in, std::string_view expected_hash = {});

    PutResult store_sync(const std::string& driver_id, std::istream& in, std::string_view expected_hash = {});
    PutResult store_sync(std::istream& in, std::string_view expected_hash = {});

    std::unique_ptr<std::istream> retrieve(const std::string& driver_id, std::string_view locator, std::optional<ByteRange> range = {});
    std::unique_ptr<std::istream> retrieve(std::string_view locator, std::optional<ByteRange> range = {});

    bool verify(const std::string& driver_id, std::string_view locator, std::string_view expected_hash);
    bool verify(std::string_view locator, std::string_view expected_hash);

    bool unlink(const std::string& driver_id, std::string_view locator);
    bool unlink(std::string_view locator);

    std::unordered_map<std::string, StorageStats> stat_all() const;

    std::string default_driver_id() const;
    void set_default_driver(const std::string& driver_id);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<IStorageDriver>> drivers_;
    std::string default_driver_id_{"default_posix_cas"};
};

} // namespace agentic_blackboard::storage

namespace ab = agentic_blackboard;
namespace blackboard = agentic_blackboard;

#include "agentic_blackboard/StorageManager.hpp"
#include <stdexcept>
#include <vector>

namespace agentic_blackboard::storage {

StorageManager::StorageManager(const std::string& default_vault_dir) {
    default_driver_id_ = "default_posix_cas";
    auto default_driver = std::make_shared<PosixCasDriver>(default_vault_dir, default_driver_id_);
    drivers_[default_driver_id_] = std::move(default_driver);
}

StorageManager::StorageManager(StorageManager&& other) noexcept {
    std::unique_lock lock(other.mutex_);
    drivers_ = std::move(other.drivers_);
    default_driver_id_ = std::move(other.default_driver_id_);
}

StorageManager& StorageManager::operator=(StorageManager&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        drivers_ = std::move(other.drivers_);
        default_driver_id_ = std::move(other.default_driver_id_);
    }
    return *this;
}

void StorageManager::register_driver(const std::string& driver_id, std::shared_ptr<IStorageDriver> driver) {
    if (driver_id.empty()) {
        throw std::invalid_argument("StorageManager::register_driver: driver_id cannot be empty");
    }
    if (!driver) {
        throw std::invalid_argument("StorageManager::register_driver: driver cannot be null");
    }
    std::unique_lock lock(mutex_);
    drivers_[driver_id] = std::move(driver);
}

bool StorageManager::has_driver(const std::string& driver_id) const {
    std::shared_lock lock(mutex_);
    return drivers_.find(driver_id) != drivers_.end();
}

std::shared_ptr<IStorageDriver> StorageManager::get_driver(const std::string& driver_id) const {
    std::shared_lock lock(mutex_);
    const std::string& target_id = driver_id.empty() ? default_driver_id_ : driver_id;
    auto it = drivers_.find(target_id);
    if (it != drivers_.end()) {
        return it->second;
    }
    return nullptr;
}

std::future<PutResult> StorageManager::store(
    const std::string& driver_id,
    std::istream& in,
    std::string_view expected_hash)
{
    auto driver = get_driver(driver_id);
    if (!driver) {
        throw std::runtime_error("StorageManager::store: driver not found: " +
                                 (driver_id.empty() ? default_driver_id() : driver_id));
    }
    return driver->put_stream(in, expected_hash);
}

std::future<PutResult> StorageManager::store(
    std::istream& in,
    std::string_view expected_hash)
{
    return store("", in, expected_hash);
}

std::unique_ptr<std::istream> StorageManager::retrieve(
    const std::string& driver_id,
    std::string_view locator,
    std::optional<ByteRange> range)
{
    if (!driver_id.empty()) {
        auto driver = get_driver(driver_id);
        if (!driver) {
            throw std::runtime_error("StorageManager::retrieve: driver not found: " + driver_id);
        }
        return driver->get_stream(locator, range);
    }

    // Fallback routing: query default driver first
    auto default_drv = get_driver("");
    if (default_drv) {
        auto stream = default_drv->get_stream(locator, range);
        if (stream) {
            return stream;
        }
    }

    // Fallback across remaining drivers
    std::shared_lock lock(mutex_);
    for (const auto& [id, driver] : drivers_) {
        if (driver && driver != default_drv) {
            auto stream = driver->get_stream(locator, range);
            if (stream) {
                return stream;
            }
        }
    }

    return nullptr;
}

std::unique_ptr<std::istream> StorageManager::retrieve(
    std::string_view locator,
    std::optional<ByteRange> range)
{
    return retrieve("", locator, range);
}

bool StorageManager::verify(
    const std::string& driver_id,
    std::string_view locator,
    std::string_view expected_hash)
{
    if (!driver_id.empty()) {
        auto driver = get_driver(driver_id);
        if (!driver) {
            throw std::runtime_error("StorageManager::verify: driver not found: " + driver_id);
        }
        return driver->verify_digest(locator, expected_hash);
    }

    // Fallback routing: check default driver first
    auto default_drv = get_driver("");
    if (default_drv && default_drv->verify_digest(locator, expected_hash)) {
        return true;
    }

    std::shared_lock lock(mutex_);
    for (const auto& [id, driver] : drivers_) {
        if (driver && driver != default_drv) {
            if (driver->verify_digest(locator, expected_hash)) {
                return true;
            }
        }
    }

    return false;
}

bool StorageManager::verify(
    std::string_view locator,
    std::string_view expected_hash)
{
    return verify("", locator, expected_hash);
}

bool StorageManager::unlink(
    const std::string& driver_id,
    std::string_view locator)
{
    if (!driver_id.empty()) {
        auto driver = get_driver(driver_id);
        if (!driver) {
            throw std::runtime_error("StorageManager::unlink: driver not found: " + driver_id);
        }
        return driver->unlink(locator);
    }

    // Fallback routing: check default driver first
    auto default_drv = get_driver("");
    if (default_drv && default_drv->unlink(locator)) {
        return true;
    }

    std::shared_lock lock(mutex_);
    for (const auto& [id, driver] : drivers_) {
        if (driver && driver != default_drv) {
            if (driver->unlink(locator)) {
                return true;
            }
        }
    }

    return false;
}

bool StorageManager::unlink(
    std::string_view locator)
{
    return unlink("", locator);
}

std::unordered_map<std::string, StorageStats> StorageManager::stat_all() const {
    std::unordered_map<std::string, StorageStats> result;
    std::vector<std::pair<std::string, std::shared_ptr<IStorageDriver>>> drivers_copy;

    {
        std::shared_lock lock(mutex_);
        drivers_copy.reserve(drivers_.size());
        for (const auto& pair : drivers_) {
            drivers_copy.push_back(pair);
        }
    }

    for (const auto& [id, driver] : drivers_copy) {
        if (driver) {
            result[id] = driver->stat();
        }
    }

    return result;
}

std::string StorageManager::default_driver_id() const {
    std::shared_lock lock(mutex_);
    return default_driver_id_;
}

void StorageManager::set_default_driver(const std::string& driver_id) {
    std::unique_lock lock(mutex_);
    if (drivers_.find(driver_id) == drivers_.end()) {
        throw std::invalid_argument("StorageManager::set_default_driver: driver not registered: " + driver_id);
    }
    default_driver_id_ = driver_id;
}

} // namespace agentic_blackboard::storage

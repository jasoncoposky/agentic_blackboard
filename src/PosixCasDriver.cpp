#include "agentic_blackboard/PosixCasDriver.hpp"
#include <openssl/evp.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <cctype>

namespace agentic_blackboard::storage {

namespace {

std::string to_hex(const unsigned char* data, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string s;
    s.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s[i * 2]     = hex_chars[(data[i] >> 4) & 0x0F];
        s[i * 2 + 1] = hex_chars[data[i] & 0x0F];
    }
    return s;
}

std::string strip_prefix(std::string_view digest) {
    auto colon = digest.find(':');
    std::string_view raw = (colon != std::string_view::npos) ? digest.substr(colon + 1) : digest;
    std::string lower;
    lower.reserve(raw.size());
    for (char c : raw) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower;
}

bool is_valid_hex(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

class RangeStreamBuf : public std::streambuf {
public:
    RangeStreamBuf(const std::filesystem::path& path, uint64_t offset, uint64_t length)
        : file_(path, std::ios::binary), remaining_(length), buffer_(64 * 1024) {
        if (file_.is_open()) {
            file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!file_) {
                remaining_ = 0;
            }
        } else {
            remaining_ = 0;
        }
    }

    bool is_open() const { return file_.is_open() && static_cast<bool>(file_); }

protected:
    int_type underflow() override {
        if (remaining_ == 0 || !file_) {
            return traits_type::eof();
        }
        size_t to_read = std::min(static_cast<uint64_t>(buffer_.size()), remaining_);
        file_.read(buffer_.data(), static_cast<std::streamsize>(to_read));
        std::streamsize bytes_read = file_.gcount();
        if (bytes_read <= 0) {
            return traits_type::eof();
        }
        remaining_ -= static_cast<uint64_t>(bytes_read);
        setg(buffer_.data(), buffer_.data(), buffer_.data() + bytes_read);
        return traits_type::to_int_type(static_cast<unsigned char>(*gptr()));
    }

private:
    std::ifstream file_;
    uint64_t remaining_{0};
    std::vector<char> buffer_;
};

struct RangeStreamBase {
    RangeStreamBuf buf;
    RangeStreamBase(const std::filesystem::path& path, uint64_t offset, uint64_t length)
        : buf(path, offset, length) {}
};

class RangeStream : private RangeStreamBase, public std::istream {
public:
    RangeStream(const std::filesystem::path& path, uint64_t offset, uint64_t length)
        : RangeStreamBase(path, offset, length), std::istream(&buf) {}

    bool is_open() const { return buf.is_open(); }
};

class TrackedStream : public std::istream {
public:
    TrackedStream(std::unique_ptr<std::istream> underlying, std::shared_ptr<std::atomic<uint32_t>> counter)
        : std::istream(underlying ? underlying->rdbuf() : nullptr),
          underlying_(std::move(underlying)),
          counter_(std::move(counter)) {
        if (counter_) {
            (*counter_)++;
        }
    }

    ~TrackedStream() override {
        if (counter_) {
            (*counter_)--;
        }
    }

    TrackedStream(const TrackedStream&) = delete;
    TrackedStream& operator=(const TrackedStream&) = delete;

private:
    std::unique_ptr<std::istream> underlying_;
    std::shared_ptr<std::atomic<uint32_t>> counter_;
};

struct StreamGuard {
    std::shared_ptr<std::atomic<uint32_t>> counter;
    ~StreamGuard() {
        if (counter) {
            (*counter)--;
        }
    }
};

struct TempFileGuard {
    std::filesystem::path path;
    bool committed{false};
    ~TempFileGuard() {
        if (!committed && !path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

} // namespace

PosixCasDriver::PosixCasDriver(std::filesystem::path vault_root, std::string driver_id)
    : vault_root_(std::move(vault_root)),
      driver_id_(std::move(driver_id)),
      active_streams_(std::make_shared<std::atomic<uint32_t>>(0)) {
    std::filesystem::create_directories(vault_root_);
    std::filesystem::create_directories(vault_root_ / ".tmp");
}

PosixCasDriver::PosixCasDriver(PosixCasDriver&& other) noexcept
    : vault_root_(std::move(other.vault_root_)),
      driver_id_(std::move(other.driver_id_)),
      active_streams_(std::move(other.active_streams_)),
      total_bytes_written_(other.total_bytes_written_.load()),
      total_write_time_ns_(other.total_write_time_ns_.load()) {}

PosixCasDriver& PosixCasDriver::operator=(PosixCasDriver&& other) noexcept {
    if (this != &other) {
        vault_root_ = std::move(other.vault_root_);
        driver_id_ = std::move(other.driver_id_);
        active_streams_ = std::move(other.active_streams_);
        total_bytes_written_.store(other.total_bytes_written_.load());
        total_write_time_ns_.store(other.total_write_time_ns_.load());
    }
    return *this;
}

std::filesystem::path PosixCasDriver::compute_target_path(std::string_view digest) const {
    std::string hex = strip_prefix(digest);
    if (!is_valid_hex(hex)) {
        throw std::invalid_argument("Invalid hex digest in compute_target_path: " + std::string(digest));
    }
    if (hex.size() < 4) {
        return vault_root_ / hex;
    }
    std::string sub1 = hex.substr(0, 2);
    std::string sub2 = hex.substr(2, 2);
    return vault_root_ / sub1 / sub2 / hex;
}

std::filesystem::path PosixCasDriver::resolve_path(std::string_view locator) const {
    if (locator.empty()) {
        throw std::invalid_argument("Empty locator provided to resolve_path");
    }

    std::error_code ec;
    std::filesystem::path canon_root = std::filesystem::weakly_canonical(vault_root_, ec);
    if (ec) {
        canon_root = std::filesystem::absolute(vault_root_);
    }

    auto is_inside_vault = [&](const std::filesystem::path& p) -> bool {
        std::error_code check_ec;
        std::filesystem::path canon_p = std::filesystem::weakly_canonical(p, check_ec);
        if (check_ec) {
            return false;
        }
        auto [root_it, _] = std::mismatch(canon_root.begin(), canon_root.end(), canon_p.begin(), canon_p.end());
        return (root_it == canon_root.end());
    };

    // Case 1: Check if locator has a known digest prefix
    bool has_digest_prefix = false;
    std::string lower_locator;
    lower_locator.reserve(locator.size());
    for (char c : locator) {
        lower_locator.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower_locator.rfind("sha256:", 0) == 0 || lower_locator.rfind("blake3:", 0) == 0) {
        has_digest_prefix = true;
    }

    if (has_digest_prefix) {
        std::filesystem::path target = compute_target_path(locator);
        if (!is_inside_vault(target)) {
            throw std::invalid_argument("Target path escapes vault root: " + target.string());
        }
        return target;
    }

    // If it is raw hex without prefix, compute target path and check existence
    if (is_valid_hex(locator)) {
        std::filesystem::path target = compute_target_path(locator);
        if (std::filesystem::is_regular_file(target, ec) && is_inside_vault(target)) {
            return target;
        }
    }

    // Case 2: Treat as filesystem path (absolute or relative)
    std::filesystem::path p(locator);
    std::filesystem::path candidate = p.is_absolute() ? p : (vault_root_ / p);

    if (!is_inside_vault(candidate)) {
        throw std::invalid_argument("Path traversal detected or path outside vault root: " + std::string(locator));
    }

    if (std::filesystem::is_regular_file(candidate, ec)) {
        return candidate;
    }

    // If candidate does not exist, but locator was valid hex, return target
    if (is_valid_hex(locator)) {
        std::filesystem::path target = compute_target_path(locator);
        if (is_inside_vault(target)) {
            return target;
        }
    }

    return candidate;
}

PutResult PosixCasDriver::put_stream_sync(std::istream& in, std::string_view expected_hash) {
    if (active_streams_) {
        (*active_streams_)++;
    }
    StreamGuard stream_guard{active_streams_};

    auto start_time = std::chrono::steady_clock::now();

    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    static std::atomic<uint64_t> counter{0};
    std::string tmp_name = "tmp_" + std::to_string(now_ns) + "_" + std::to_string(counter++) + ".tmp";
    std::filesystem::path tmp_dir = vault_root_ / ".tmp";
    std::filesystem::create_directories(tmp_dir);
    std::filesystem::path tmp_path = tmp_dir / tmp_name;

    TempFileGuard guard{tmp_path};

    std::ofstream out(tmp_path, std::ios::binary);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to create temporary CAS file: " + tmp_path.string());
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
        throw std::runtime_error("Failed to allocate EVP_MD_CTX");
    }
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("Failed to initialize OpenSSL EVP digest");
    }

    char buffer[65536];
    uint64_t bytes_written = 0;
    while (in) {
        in.read(buffer, sizeof(buffer));
        std::streamsize bytes = in.gcount();
        if (bytes > 0) {
            out.write(buffer, bytes);
            if (!out) {
                throw std::runtime_error("Failed writing to temporary CAS file: " + tmp_path.string());
            }
            if (EVP_DigestUpdate(ctx.get(), buffer, static_cast<size_t>(bytes)) != 1) {
                throw std::runtime_error("Failed updating OpenSSL digest");
            }
            bytes_written += static_cast<uint64_t>(bytes);
        }
    }

    if (in.bad() || (!in.eof() && in.fail())) {
        throw std::runtime_error("I/O failure while reading input stream for CAS storage");
    }

    out.flush();
    out.close();
    if (!out) {
        throw std::runtime_error("Failed closing temporary CAS file: " + tmp_path.string());
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash, &hash_len) != 1) {
        throw std::runtime_error("Failed finalizing OpenSSL digest");
    }

    std::string hex_hash = to_hex(hash, hash_len);
    std::string prefix = "sha256:";

    if (!expected_hash.empty()) {
        std::string lower_expected;
        lower_expected.reserve(expected_hash.size());
        for (char c : expected_hash) {
            lower_expected.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower_expected.rfind("blake3:", 0) == 0) {
            prefix = "blake3:";
        }
        std::string expected_hex = strip_prefix(expected_hash);
        if (expected_hex != hex_hash) {
            throw std::runtime_error("Digest mismatch: expected " + std::string(expected_hash) + " but got " + prefix + hex_hash);
        }
    }

    std::string digest = prefix + hex_hash;

    std::filesystem::path target_path = compute_target_path(digest);
    std::filesystem::create_directories(target_path.parent_path());

    if (std::filesystem::exists(target_path)) {
        // CAS payload deduplication: file already exists
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        guard.committed = true;
    } else {
        std::error_code ec;
        std::filesystem::rename(tmp_path, target_path, ec);
        if (ec) {
            if (std::filesystem::exists(target_path)) {
                std::filesystem::remove(tmp_path, ec);
                guard.committed = true;
            } else {
                throw std::runtime_error("Failed to rename temporary CAS file to target: " + ec.message());
            }
        } else {
            guard.committed = true;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    total_bytes_written_ += bytes_written;
    total_write_time_ns_ += static_cast<uint64_t>(duration_ns);

    uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    PutResult result;
    result.digest = digest;
    result.bytes_written = bytes_written;
    result.driver_id = driver_id_;
    result.locator = target_path.string();
    result.timestamp_ms = now_ms;
    return result;
}

auto PosixCasDriver::put_stream(std::istream& in, std::string_view expected_hash) 
    -> std::future<PutResult> 
{
    return std::async(std::launch::async, [this, &in, expected = std::string(expected_hash)]() {
        return put_stream_sync(in, expected);
    });
}

auto PosixCasDriver::get_stream(std::string_view locator, std::optional<ByteRange> range) 
    -> std::unique_ptr<std::istream> 
{
    std::filesystem::path path;
    try {
        path = resolve_path(locator);
    } catch (const std::invalid_argument&) {
        return nullptr;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return nullptr;
    }
    uint64_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        return nullptr;
    }

    if (!range.has_value()) {
        auto file = std::make_unique<std::ifstream>(path, std::ios::binary);
        if (!file->is_open()) {
            return nullptr;
        }
        return std::make_unique<TrackedStream>(std::move(file), active_streams_);
    }

    if (range->offset > file_size) {
        return nullptr;
    }
    uint64_t actual_length = std::min(range->length, file_size - range->offset);
    auto range_stream = std::make_unique<RangeStream>(path, range->offset, actual_length);
    if (!range_stream->is_open()) {
        return nullptr;
    }
    return std::make_unique<TrackedStream>(std::move(range_stream), active_streams_);
}

bool PosixCasDriver::verify_digest(std::string_view locator, std::string_view expected_hash) {
    std::filesystem::path path;
    try {
        path = resolve_path(locator);
    } catch (const std::invalid_argument&) {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
        return false;
    }
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return false;
    }

    char buffer[65536];
    while (in) {
        in.read(buffer, sizeof(buffer));
        std::streamsize bytes = in.gcount();
        if (bytes > 0) {
            if (EVP_DigestUpdate(ctx.get(), buffer, static_cast<size_t>(bytes)) != 1) {
                return false;
            }
        }
    }
    if (in.bad() || (!in.eof() && in.fail())) {
        return false;
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash, &hash_len) != 1) {
        return false;
    }

    std::string hex_hash = to_hex(hash, hash_len);
    std::string expected_hex = strip_prefix(expected_hash);

    return (expected_hex == hex_hash);
}

bool PosixCasDriver::unlink(std::string_view locator) {
    std::filesystem::path path;
    try {
        path = resolve_path(locator);
    } catch (const std::invalid_argument&) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    return std::filesystem::remove(path, ec);
}

StorageStats PosixCasDriver::stat() {
    StorageStats stats;
    std::error_code ec;
    auto space_info = std::filesystem::space(vault_root_, ec);
    if (!ec) {
        stats.total_capacity_bytes = space_info.capacity;
        stats.free_capacity_bytes = space_info.available;
    }
    stats.active_streams = active_streams_ ? active_streams_->load() : 0;
    uint64_t bytes = total_bytes_written_.load();
    uint64_t time_ns = total_write_time_ns_.load();
    if (time_ns > 0) {
        double seconds = static_cast<double>(time_ns) / 1e9;
        double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        stats.write_throughput_mb_s = mb / seconds;
    } else {
        stats.write_throughput_mb_s = 0.0;
    }
    return stats;
}

} // namespace agentic_blackboard::storage

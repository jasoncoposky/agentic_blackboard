#include "agentic_blackboard/PosixCasDriver.hpp"
#include <openssl/evp.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace blackboard::storage {

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

std::string_view strip_prefix(std::string_view digest) {
    auto colon = digest.find(':');
    if (colon != std::string_view::npos) {
        return digest.substr(colon + 1);
    }
    return digest;
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

    bool is_open() const { return file_.is_open(); }

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

struct StreamGuard {
    std::atomic<uint32_t>& count;
    ~StreamGuard() { count--; }
};

} // namespace

PosixCasDriver::PosixCasDriver(std::filesystem::path vault_root, std::string driver_id)
    : vault_root_(std::move(vault_root)), driver_id_(std::move(driver_id)) {
    std::filesystem::create_directories(vault_root_);
    std::filesystem::create_directories(vault_root_ / ".tmp");
}

PosixCasDriver::PosixCasDriver(PosixCasDriver&& other) noexcept
    : vault_root_(std::move(other.vault_root_)),
      driver_id_(std::move(other.driver_id_)),
      active_streams_(other.active_streams_.load()),
      total_bytes_written_(other.total_bytes_written_.load()),
      total_write_time_ns_(other.total_write_time_ns_.load()) {}

PosixCasDriver& PosixCasDriver::operator=(PosixCasDriver&& other) noexcept {
    if (this != &other) {
        vault_root_ = std::move(other.vault_root_);
        driver_id_ = std::move(other.driver_id_);
        active_streams_.store(other.active_streams_.load());
        total_bytes_written_.store(other.total_bytes_written_.load());
        total_write_time_ns_.store(other.total_write_time_ns_.load());
    }
    return *this;
}

std::filesystem::path PosixCasDriver::compute_target_path(std::string_view digest) const {
    std::string_view hex = strip_prefix(digest);
    if (hex.size() < 4) {
        return vault_root_ / hex;
    }
    std::string sub1(hex.substr(0, 2));
    std::string sub2(hex.substr(2, 2));
    return vault_root_ / sub1 / sub2 / hex;
}

std::filesystem::path PosixCasDriver::resolve_path(std::string_view locator) const {
    std::filesystem::path p(locator);
    if (std::filesystem::exists(p)) {
        return p;
    }
    std::filesystem::path rel = vault_root_ / p;
    if (std::filesystem::exists(rel)) {
        return rel;
    }
    std::filesystem::path target = compute_target_path(locator);
    if (std::filesystem::exists(target)) {
        return target;
    }
    return p;
}

PutResult PosixCasDriver::put_stream_sync(std::istream& in, std::string_view expected_hash) {
    active_streams_++;
    StreamGuard stream_guard{active_streams_};

    auto start_time = std::chrono::steady_clock::now();

    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    static std::atomic<uint64_t> counter{0};
    std::string tmp_name = "tmp_" + std::to_string(now_ns) + "_" + std::to_string(counter++) + ".tmp";
    std::filesystem::path tmp_dir = vault_root_ / ".tmp";
    std::filesystem::create_directories(tmp_dir);
    std::filesystem::path tmp_path = tmp_dir / tmp_name;

    bool committed = false;
    auto cleanup = [&]() {
        if (!committed) {
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
        }
    };

    std::ofstream out(tmp_path, std::ios::binary);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to create temporary CAS file: " + tmp_path.string());
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        cleanup();
        throw std::runtime_error("Failed to allocate EVP_MD_CTX");
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        cleanup();
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
                EVP_MD_CTX_free(ctx);
                cleanup();
                throw std::runtime_error("Failed writing to temporary CAS file: " + tmp_path.string());
            }
            if (EVP_DigestUpdate(ctx, buffer, static_cast<size_t>(bytes)) != 1) {
                EVP_MD_CTX_free(ctx);
                cleanup();
                throw std::runtime_error("Failed updating OpenSSL digest");
            }
            bytes_written += static_cast<uint64_t>(bytes);
        }
    }
    out.flush();
    out.close();
    if (!out) {
        EVP_MD_CTX_free(ctx);
        cleanup();
        throw std::runtime_error("Failed closing temporary CAS file: " + tmp_path.string());
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        cleanup();
        throw std::runtime_error("Failed finalizing OpenSSL digest");
    }
    EVP_MD_CTX_free(ctx);

    std::string hex_hash = to_hex(hash, hash_len);
    std::string digest = "blake3:" + hex_hash;

    if (!expected_hash.empty()) {
        std::string_view expected_hex = strip_prefix(expected_hash);
        if (expected_hex != hex_hash) {
            cleanup();
            throw std::runtime_error("Digest mismatch: expected " + std::string(expected_hash) + " but got " + digest);
        }
    }

    std::filesystem::path target_path = compute_target_path(digest);
    std::filesystem::create_directories(target_path.parent_path());

    if (std::filesystem::exists(target_path)) {
        // CAS payload deduplication: file already exists
        cleanup();
        committed = true;
    } else {
        std::error_code ec;
        std::filesystem::rename(tmp_path, target_path, ec);
        if (ec) {
            if (std::filesystem::exists(target_path)) {
                cleanup();
                committed = true;
            } else {
                cleanup();
                throw std::runtime_error("Failed to rename temporary CAS file to target: " + ec.message());
            }
        } else {
            committed = true;
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
    std::filesystem::path path = resolve_path(locator);
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
        return file;
    }

    if (range->offset > file_size) {
        return nullptr;
    }
    uint64_t actual_length = std::min(range->length, file_size - range->offset);
    auto range_stream = std::make_unique<RangeStream>(path, range->offset, actual_length);
    if (!range_stream->is_open()) {
        return nullptr;
    }
    return range_stream;
}

bool PosixCasDriver::verify_digest(std::string_view locator, std::string_view expected_hash) {
    std::filesystem::path path = resolve_path(locator);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }

    char buffer[65536];
    while (in) {
        in.read(buffer, sizeof(buffer));
        std::streamsize bytes = in.gcount();
        if (bytes > 0) {
            if (EVP_DigestUpdate(ctx, buffer, static_cast<size_t>(bytes)) != 1) {
                EVP_MD_CTX_free(ctx);
                return false;
            }
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    EVP_MD_CTX_free(ctx);

    std::string hex_hash = to_hex(hash, hash_len);
    std::string blake3_digest = "blake3:" + hex_hash;
    std::string sha256_digest = "sha256:" + hex_hash;

    return (expected_hash == blake3_digest || expected_hash == hex_hash || expected_hash == sha256_digest);
}

bool PosixCasDriver::unlink(std::string_view locator) {
    std::filesystem::path path = resolve_path(locator);
    std::error_code ec;
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
    stats.active_streams = active_streams_.load();
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

} // namespace blackboard::storage

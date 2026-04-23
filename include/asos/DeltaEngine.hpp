#pragma once

#include "buffer.hpp"
#include "asos/schema.hpp"
#include <vector>
#include <cstdint>

namespace asos {

/**
 * @brief Utility for computing and applying binary deltas for CPB Atoms.
 */
class DeltaEngine {
public:
    /**
     * @brief Compute a binary XOR patch between base and target.
     * @note For v0.2, assumes buffers are reasonably similar in size.
     */
    static L3DeltaPatch create_xor_patch(const lite3cpp::Buffer& base, const lite3cpp::Buffer& target);

    /**
     * @brief Apply an XOR patch to a base buffer to reconstruct the target.
     * @return Reconstructed buffer or empty optional on checksum failure.
     */
    static std::optional<lite3cpp::Buffer> apply_xor_patch(const lite3cpp::Buffer& base, const L3DeltaPatch& patch);

    /**
     * @brief Calculate XXH3_64bits checksum of a buffer.
     */
    static uint64_t calculate_checksum(const lite3cpp::Buffer& buf);
};

} // namespace asos

#include "asos/DeltaEngine.hpp"
#include <xxhash.h>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace asos {

uint64_t DeltaEngine::calculate_checksum(const lite3cpp::Buffer& buf) {
    return XXH3_64bits(buf.data(), buf.size());
}

L3DeltaPatch DeltaEngine::create_xor_patch(const lite3cpp::Buffer& base, const lite3cpp::Buffer& target) {
    L3DeltaPatch patch;
    
    // For v0.2 XOR patch, we'll patch from offset 0 up to the max size
    // and zero-pad the shorter buffer if sizes differ.
    // In a more complex version, we could find the first and last diff offsets.
    
    size_t base_size = base.size();
    size_t target_size = target.size();
    size_t max_size = std::max(base_size, target_size);
    
    patch.patch.offset = 0;
    patch.patch.binary_delta.resize(max_size);
    
    for (size_t i = 0; i < max_size; ++i) {
        uint8_t base_byte = (i < base_size) ? base.data()[i] : 0;
        uint8_t target_byte = (i < target_size) ? target.data()[i] : 0;
        patch.patch.binary_delta[i] = base_byte ^ target_byte;
    }
    
    patch.patch.checksum = calculate_checksum(target);
    return patch;
}

std::optional<lite3cpp::Buffer> DeltaEngine::apply_xor_patch(const lite3cpp::Buffer& base, const L3DeltaPatch& patch) {
    size_t base_size = base.size();
    size_t delta_size = patch.patch.binary_delta.size();
    
    // Reconstruct target size calculation (same as delta_size for XOR)
    size_t target_size = delta_size;
    
    std::vector<uint8_t> reconstructed_data(target_size);
    
    for (size_t i = 0; i < target_size; ++i) {
        uint8_t base_byte = (i < base_size) ? base.data()[i] : 0;
        uint8_t delta_byte = patch.patch.binary_delta[i];
        reconstructed_data[i] = base_byte ^ delta_byte;
    }
    
    lite3cpp::Buffer reconstructed(reconstructed_data);
    
    // Verify Checksum
    uint64_t current_checksum = calculate_checksum(reconstructed);
    if (current_checksum != patch.patch.checksum) {
        std::cerr << "[DeltaEngine] Checksum mismatch! Expected: " << patch.patch.checksum 
                  << " Got: " << current_checksum << std::endl;
        return std::nullopt;
    }
    
    return reconstructed;
}

} // namespace asos

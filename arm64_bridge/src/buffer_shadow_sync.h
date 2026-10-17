#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Compare CPU-accessible shadows, never read the potentially uncached GPU
// mapping. A first synchronization must be forced even if both shadows are zero:
// glBufferStorage(NULL) need not have initialized the native storage.
inline size_t SyncChangedBufferChunks(void* native_mapping, void* cpu_shadow,
                                      const void* guest_data, size_t size,
                                      bool force_full) {
    auto* native = static_cast<uint8_t*>(native_mapping);
    auto* shadow = static_cast<uint8_t*>(cpu_shadow);
    const auto* guest = static_cast<const uint8_t*>(guest_data);
    constexpr size_t chunk_size = 4096;
    // Most persistent mappings are unchanged between adjacent draw calls.
    // A single contiguous comparison avoids hundreds of 4 KiB memcmp calls
    // in that common case, while keeping the per-chunk copy path for writes.
    if (!force_full && std::memcmp(shadow, guest, size) == 0) return 0;
    size_t copied = 0;
    for (size_t offset = 0; offset < size;) {
        const size_t count = std::min(chunk_size, size - offset);
        if (force_full || std::memcmp(shadow + offset, guest + offset, count) != 0) {
            std::memcpy(shadow + offset, guest + offset, count);
            // Publish the same snapshot that future comparisons remember.
            // A producer can change guest memory between two copies; reading
            // it again would let the shadow advance without updating the GPU.
            std::memcpy(native + offset, shadow + offset, count);
            copied += count;
        }
        offset += count;
    }
    return copied;
}

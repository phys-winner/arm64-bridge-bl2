#pragma once

#include <cstdint>

namespace bl2::gl {

struct StorageCallDiagnostic {
    bool seen = false;
    uint32_t pc = 0, lr = 0, buffer = 0, data = 0;
    uint32_t flags = 0, native_flags = 0;
    int64_t size = 0;
    bool native_ext = false;
};

// Failure provenance must not add queries, logging, or cross-thread locking
// to an otherwise successful storage/map operation.
inline constinit thread_local StorageCallDiagnostic last_array_storage_call{};
inline constinit thread_local StorageCallDiagnostic last_element_storage_call{};

inline StorageCallDiagnostic* LastStorageCallDiagnostic(uint32_t target) {
    if (target == 0x8892) return &last_array_storage_call;   // GL_ARRAY_BUFFER
    if (target == 0x8893) return &last_element_storage_call; // GL_ELEMENT_ARRAY_BUFFER
    return nullptr;
}

} // namespace bl2::gl

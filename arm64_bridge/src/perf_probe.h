#pragma once

#include <cstddef>
#include <cstdint>

enum class HornPerfCategory : uint8_t {
    Swap,
    Draw,
    ShaderCompile,
    ShaderLink,
    TextureUpload,
    Mipmap,
    BufferUpload,
    GpuSync,
    FileRead,
    FileWrite,
    FileSync,
    Decompress,
    GuestAlloc,
    JavaCall,
    Count,
};

#ifndef HORN_ENABLE_PERF_PROBE
#define HORN_ENABLE_PERF_PROBE 0
#endif

#if HORN_ENABLE_PERF_PROBE

bool HornPerfEnabled();
uint64_t HornPerfNowNs();
void HornPerfRecord(HornPerfCategory category, uint64_t elapsed_ns,
                    uint64_t bytes = 0);

// Called by Dynarmic's translator hook once per newly translated guest
// instruction. It is intentionally separate from executed instructions: a
// spike here identifies cold JIT work inside an otherwise unclassified frame.
void HornPerfRecordTranslation(uint32_t guest_pc);

// Called when a property-gated Dynarmic instruction budget expires. Sampling
// at coarse block boundaries identifies hot guest code without requiring
// ptrace/perf access to the translated ARM64 code cache.
void HornPerfRecordGuestSample(uint32_t guest_pc);

// Call immediately after JavaCallback_swapBuffers returns. This is the one
// stable frame boundary exposed by the original ARM32 game.
void HornPerfFrameBoundary();

class HornPerfScope {
public:
    explicit HornPerfScope(HornPerfCategory category, uint64_t bytes = 0)
        : category_(category), bytes_(bytes),
          start_ns_(HornPerfEnabled() ? HornPerfNowNs() : 0) {}

    ~HornPerfScope() {
        if (start_ns_ != 0) {
            HornPerfRecord(category_, HornPerfNowNs() - start_ns_, bytes_);
        }
    }

    HornPerfScope(const HornPerfScope&) = delete;
    HornPerfScope& operator=(const HornPerfScope&) = delete;

    void SetBytes(uint64_t bytes) { bytes_ = bytes; }

private:
    HornPerfCategory category_;
    uint64_t bytes_;
    uint64_t start_ns_;
};

#else

inline constexpr bool HornPerfEnabled() { return false; }
inline constexpr uint64_t HornPerfNowNs() { return 0; }
inline void HornPerfRecord(HornPerfCategory, uint64_t, uint64_t = 0) {}
inline void HornPerfRecordTranslation(uint32_t) {}
inline void HornPerfRecordGuestSample(uint32_t) {}
inline void HornPerfFrameBoundary() {}

class HornPerfScope {
public:
    explicit HornPerfScope(HornPerfCategory, uint64_t = 0) {}

    HornPerfScope(const HornPerfScope&) = delete;
    HornPerfScope& operator=(const HornPerfScope&) = delete;

    void SetBytes(uint64_t) {}
};

#endif

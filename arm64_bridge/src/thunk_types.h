#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <algorithm>
#include <stdexcept>
#include <dynarmic/interface/A32/a32.h>
#include "dynarmic_host.h"

// Helper to convert 32-bit guest pointer to 64-bit host pointer
inline void* GuestToHost(uint8_t* base, uint32_t gaddr) {
    if (gaddr == 0) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + gaddr);
}

// Helper to convert 64-bit host pointer into 32-bit guest address (if within guest arena)
inline uint32_t HostToGuest(uint8_t* base, void* hptr) {
    if (!hptr) return 0;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hptr) - reinterpret_cast<uintptr_t>(base));
}

// True when a guest data range starts in the guarded bottom pages (exit traps,
// SVC trampolines). Native Android maps nothing there: only a NULL-based
// pointer can name it, so thunks treat it like the kernel does (EFAULT).
inline bool GuestLowGuardHit(uint32_t gaddr, size_t size) {
#if defined(BL2_NATIVE_ARM32)
    (void)gaddr;
    (void)size;
    return false;
#else
    return size != 0 && gaddr < DynarmicHost::kLowGuardEnd;
#endif
}

inline bool GuestRangeValid(size_t arena_size, uint32_t gaddr, size_t size) {
    if (GuestLowGuardHit(gaddr, size)) return false;
    return gaddr != 0 ? (gaddr <= arena_size && size <= arena_size - gaddr)
                      : size == 0;
}

inline void* GuestSpan(uint8_t* base, size_t arena_size, uint32_t gaddr,
                       size_t size) {
    return GuestRangeValid(arena_size, gaddr, size)
        ? reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + gaddr) : nullptr;
}

inline const char* GuestCString(uint8_t* base, size_t arena_size,
                                uint32_t gaddr) {
    constexpr size_t kMaxGuestCString = 1024 * 1024;
    if (!GuestRangeValid(arena_size, gaddr, 1)) return nullptr;
    const char* value = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(base) + gaddr);
    const size_t limit = std::min(arena_size - gaddr, kMaxGuestCString);
    return std::memchr(value, 0, limit) ? value : nullptr;
}

class Arm32CallArgs {
public:
    Arm32CallArgs(Dynarmic::A32::Jit& jit, DynarmicHost& host,
                  uint32_t first_word = 0)
        : jit_(jit), host_(&host), base_(host.GetGuestMemoryBase()),
          word_(first_word) {}

    uint32_t U32() { return Word(word_++); }
    int32_t I32() { return static_cast<int32_t>(U32()); }
    uint32_t Ptr() { return U32(); }
    uint64_t U64() {
        word_ = (word_ + 1u) & ~1u;
        const uint64_t low = Word(word_++);
        return low | (static_cast<uint64_t>(Word(word_++)) << 32);
    }
    int64_t I64() { return static_cast<int64_t>(U64()); }
    float F32() {
        const uint32_t raw = U32();
        float value;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }
    double F64() {
        const uint64_t raw = U64();
        double value;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

private:
    uint32_t Word(uint32_t index) const {
        if (index < 4) return jit_.Regs()[index];
        const uint64_t address = static_cast<uint64_t>(jit_.Regs()[13]) +
                                 static_cast<uint64_t>(index - 4u) * 4;
        if (address + 4 > DynarmicHost::GUEST_ADDRESS_SPACE) {
            throw std::out_of_range("ARM32 guest argument overflow");
        }
        if (host_) return host_->MemoryRead32(static_cast<uint32_t>(address));
        uint32_t value;
        std::memcpy(&value, base_ + address, sizeof(value));
        return value;
    }

    Dynarmic::A32::Jit& jit_;
    DynarmicHost* host_;
    uint8_t* base_;
    uint32_t word_;
};

// -------------------------------------------------------------
// Soft-float-ABI (base AAPCS) helpers.
// Check the guest first: `llvm-readelf -A <guest.so> | grep Tag_ABI_VFP_args`.
// No tag means -mfloat-abi=softfp -- VFP is used for arithmetic, but FP
// *arguments and results* travel in the core registers:
//   float  arg N  -> r[N]                     (r0..r3, then the stack)
//   double arg N  -> r[2N]:r[2N+1] lo:hi      (even-aligned pair)
//   float  return -> r0,  double return -> r0:r1
// Confirm by hand in the disassembly: a softfp guest does `VMOV R0, S14 /
// BL sqrtf / VMOV S15, R0`, a hardfp one already has the value in s0. Getting
// this wrong does not crash anything -- it silently corrupts every float, and
// surfaces a week later as "the animation does not play".
//
// Mixed signatures must be checked one by one: glTexParameterf takes its float
// in r2, glUniform1f in r1, glUniform4f pushes the fourth float to the stack,
// frexp/ldexp take an int argument in r2.
//
// `idx` is the argument slot, so the index arithmetic is the same as it was
// for the VFP register file — only the register bank changes.
// -------------------------------------------------------------

inline float ReadSF(Dynarmic::A32::Jit& jit, int idx) {
    float v;
    uint32_t raw = jit.Regs()[idx];
    memcpy(&v, &raw, 4);
    return v;
}

inline void WriteSF(Dynarmic::A32::Jit& jit, int idx, float v) {
    uint32_t raw;
    memcpy(&raw, &v, 4);
    jit.Regs()[idx] = raw;
}

inline double ReadDF(Dynarmic::A32::Jit& jit, int d_idx) {
    uint64_t raw = static_cast<uint64_t>(jit.Regs()[d_idx * 2]) |
                   (static_cast<uint64_t>(jit.Regs()[d_idx * 2 + 1]) << 32);
    double v;
    memcpy(&v, &raw, 8);
    return v;
}

inline void WriteDF(Dynarmic::A32::Jit& jit, int d_idx, double v) {
    uint64_t raw;
    memcpy(&raw, &v, 8);
    jit.Regs()[d_idx * 2] = static_cast<uint32_t>(raw & 0xFFFFFFFFu);
    jit.Regs()[d_idx * 2 + 1] = static_cast<uint32_t>(raw >> 32);
}

// Argument slots past r3 live on the guest stack at [sp + byte_off].
inline float ReadStackF(Dynarmic::A32::Jit& jit, uint8_t* guest_base, int byte_off) {
    uint32_t sp = jit.Regs()[13];
    float v;
    memcpy(&v, guest_base + sp + byte_off, 4);
    return v;
}

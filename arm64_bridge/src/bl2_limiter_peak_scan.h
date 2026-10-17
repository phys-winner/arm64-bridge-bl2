#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Exact scalar effect of the ten original ARM instructions at limiter+0x2AC.
// This only reads the caller's already verified anonymous DSP memory bank.
// The caller must verify the pinned guest function before installing a hook.
namespace Bl2LimiterPeak {
inline constexpr std::uint32_t kStateSize = 0x1E10;
inline constexpr char kThunkName[] = "bl2_limiter_peak_scan";

inline bool Try(const std::uint8_t* memory, std::size_t memory_size,
                std::array<std::uint32_t, 16>& regs,
                std::array<std::uint32_t, 64>& ext,
                std::uint32_t& cpsr, std::uint32_t& fpscr,
                std::uint32_t continuation) noexcept {
    using U32 = std::uint32_t;
    using U64 = std::uint64_t;
    // ARM, little endian, outside IT/Jazelle, LEN=1/STRIDE=1. Exception
    // enables are unsupported; the normal Jit API already masks these bits.
    if (!memory || (cpsr & 0x0700FE20u) || (fpscr & 0x00379F00u)) return false;
    const U32 limit = regs[2], index = regs[8], address = regs[7];
    const U64 bank_begin = regs[0], bank_end = bank_begin + kStateSize;
    if (!limit || index >= limit || limit > kStateSize / 4 || (address & 3) ||
        bank_end > memory_size || address < bank_begin + 96 ||
        U64(address) + U64(limit - index) * 4 > bank_end) return false;

    // Positive IEEE f32 encodings have numerical integer order. Keep raw
    // ABS/move bits, including NaN payloads and subnormals; FZ affects only
    // comparison operands. VCMPE raises IOC for either quiet or signaling
    // NaN. Input denorms with FZ raise IDC even if the other operand is NaN.
    const bool flush = (fpscr & 0x01000000u) != 0;
    U32 selected = ext[12], last = 0, absolute = 0, peak = 0, flags = 0;
    U32 sticky = fpscr;
    const auto compare_operand = [&](U32 bits) {
        if (flush && bits != 0 && bits < 0x00800000u) {
            sticky |= 0x80u;
            return U32{0};
        }
        return bits;
    };
    for (U32 i = index; i < limit; ++i) {
        peak = selected & 0x7FFFFFFFu;
        std::memcpy(&last, memory + U64(address) + U64(i - index) * 4, 4);
        absolute = last & 0x7FFFFFFFu;
        const U32 lhs = compare_operand(absolute), rhs = compare_operand(peak);
        if (lhs > 0x7F800000u || rhs > 0x7F800000u) {
            flags = 0x30000000u;
            sticky |= 1u;
        } else if (lhs < rhs) {
            flags = 0x80000000u;
        } else if (lhs == rhs) {
            flags = 0x60000000u;
        } else {
            flags = 0x20000000u;
            peak = absolute;
            selected = last;
        }
    }
    ext[12] = selected;
    ext[13] = absolute;
    ext[14] = last;
    ext[15] = peak;
    regs[7] = address + (limit - index) * 4;
    regs[8] = limit;
    regs[15] = continuation;
    // The final CMP r8,r2 is equal. VMRS changed only the previous NZCV;
    // Q/GE/mode/other CPSR bits and all other FPSCR fields remain intact.
    cpsr = (cpsr & 0x0FFFFFFFu) | 0x60000000u;
    fpscr = (sticky & 0x0FFFFFFFu) | flags;
    return true;
}
}  // namespace Bl2LimiterPeak

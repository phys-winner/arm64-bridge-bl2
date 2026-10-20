#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bl2::lan {
constexpr std::size_t kLanReceiveLimit = 512, kV7Header = 16, kV8Header = 23;
using Packet = std::array<std::uint8_t, kLanReceiveLimit + kV8Header - kV7Header>;
inline std::uint32_t Be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
inline std::uint64_t Be64(const std::uint8_t* p) { return (std::uint64_t(Be32(p)) << 32) | Be32(p + 4); }
inline void Put32(std::uint8_t* p, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (24 - i*8));
}
inline void Put64(std::uint8_t* p, std::uint64_t v) { Put32(p, std::uint32_t(v >> 32)); Put32(p + 4, std::uint32_t(v)); }
inline bool IsV7(std::span<const std::uint8_t> p, std::uint32_t game, bool query, std::uint64_t nonce) {
    return p.size() >= kV7Header && p.size() <= kLanReceiveLimit && p[0] == 7 && (p[1] & 0x41) &&
        Be32(p.data() + 2) == game && p[6] == 'S' && p[7] == (query ? 'Q' : 'R') &&
        (query ? p.size() == kV7Header : p.size() > kV7Header && Be64(p.data() + 8) == nonce);
}
inline std::size_t NormalizeV7(std::span<const std::uint8_t> p, Packet& out, std::uint32_t game,
                              bool query, std::uint64_t nonce, std::uint32_t localBuild) {
    if (!IsV7(p, game, query, nonce)) return 0;
    out[0] = 8; Put32(out.data() + 1, p[1]); Put32(out.data() + 5, game);
    out[9] = 'S'; out[10] = query ? 'Q' : 'R'; Put32(out.data() + 11, localBuild);
    for (std::size_t i = 8; i < p.size(); ++i) out[i + 7] = p[i];
    return p.size() + 7;
}
inline std::size_t EmitV7(std::span<const std::uint8_t> p, Packet& out, std::uint32_t game, bool query) {
    if (p.size() < kV8Header || p.size() > out.size() || p[0] != 8 ||
        !(Be32(p.data() + 1) & 0x241) || Be32(p.data() + 5) != game || p[9] != 'S' ||
        p[10] != (query ? 'Q' : 'R') || (query ? p.size() != kV8Header : p.size() == kV8Header)) return 0;
    out[0] = 7; out[1] = 1; Put32(out.data() + 2, game); out[6] = 'S'; out[7] = query ? 'Q' : 'R';
    for (std::size_t i = 15; i < p.size(); ++i) out[i - 7] = p[i];
    return p.size() - 7;
}
} // namespace bl2::lan

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

// This catalog pins the original guest already verified by the JNI loader.
// ponytail: default-mode ARM paths only; capture other modes if profiling needs them.
class Bl2JitWarmup {
public:
    enum Role : size_t { Main, Callback, Renderer, RoleCount };
    static constexpr size_t kMaxBytes = 512u * 1024u;
    static constexpr uint32_t kGuestBytes = 49885152;
    static constexpr std::array<uint8_t, 32> kGuestSha{
        0xA0, 0xB0, 0x6E, 0xF5, 0x9F, 0x4C, 0x89, 0xC0,
        0x88, 0x96, 0x54, 0x19, 0x69, 0x51, 0xD2, 0xDE,
        0xB9, 0x66, 0xCF, 0xEA, 0xC7, 0x18, 0x06, 0x99,
        0xB5, 0x72, 0x4F, 0x0E, 0x26, 0x93, 0x88, 0x14};

    template<class ReadOnlyCode>
    bool Parse(std::span<const uint8_t> bytes, ReadOnlyCode read_only_code) {
        constexpr size_t header = 56;
        if (bytes.size() < header || bytes.size() > kMaxBytes ||
            std::memcmp(bytes.data(), "BL2JIT01", 8) != 0 ||
            Read32(bytes.data() + 8) != kGuestBytes ||
            !std::equal(kGuestSha.begin(), kGuestSha.end(), bytes.begin() + 12))
            return false;
        std::array<uint32_t, RoleCount> counts{};
        size_t total = 0;
        for (size_t role = 0; role < RoleCount; ++role) {
            counts[role] = Read32(bytes.data() + 44 + 4 * role);
            if (!counts[role] || counts[role] > 32768) return false;
            total += counts[role];
        }
        if (bytes.size() != header + 4 * total) return false;
        std::array<std::vector<uint32_t>, RoleCount> candidate;
        size_t offset = header;
        try {
            for (size_t role = 0; role < RoleCount; ++role) {
                candidate[role].reserve(counts[role]);
                uint32_t previous = 0;
                for (uint32_t i = 0; i < counts[role]; ++i, offset += 4) {
                    const uint32_t pc = Read32(bytes.data() + offset);
                    if ((pc & 3u) || pc <= previous || !read_only_code(pc))
                        return false;
                    candidate[role].push_back(pc);
                    previous = pc;
                }
            }
        } catch (...) {
            return false;
        }
        pcs_ = std::move(candidate);
        return true;
    }

    const std::vector<uint32_t>& PCs(Role role) const { return pcs_[role]; }

private:
    static uint32_t Read32(const uint8_t* bytes) {
        return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) |
               (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[3]} << 24);
    }
    std::array<std::vector<uint32_t>, RoleCount> pcs_;
};

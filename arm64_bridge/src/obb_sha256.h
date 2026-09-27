#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

// Small streaming SHA-256 for direct OBB integrity checks. It keeps at most
// one 64-byte block, so a whole-container split never needs to be extracted.
class ObbSha256 {
public:
    using Digest = std::array<uint8_t, 32>;

    constexpr bool Update(const uint8_t* data, size_t length) {
        if (finalized_ || invalid_ || (!data && length != 0) ||
            length > std::numeric_limits<uint64_t>::max() / 8 - total_bytes_) {
            invalid_ = true;
            return false;
        }
        total_bytes_ += length;
        if (!std::is_constant_evaluated()) {
            while (length != 0) {
                const size_t capacity = block_.size() - block_used_;
                const size_t count = length < capacity ? length : capacity;
                std::memcpy(block_.data() + block_used_, data, count);
                data += count;
                length -= count;
                block_used_ += count;
                if (block_used_ == block_.size()) {
                    Transform();
                    block_used_ = 0;
                }
            }
            return true;
        }
        for (size_t i = 0; i < length; ++i) {
            block_[block_used_++] = data[i];
            if (block_used_ == block_.size()) {
                Transform();
                block_used_ = 0;
            }
        }
        return true;
    }

    constexpr Digest Finalize() {
        if (finalized_) return digest_;
        if (invalid_) return {};
        const uint64_t bit_count = total_bytes_ * 8;
        block_[block_used_++] = 0x80;
        if (block_used_ > 56) {
            while (block_used_ < block_.size()) block_[block_used_++] = 0;
            Transform();
            block_used_ = 0;
        }
        while (block_used_ < 56) block_[block_used_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[block_used_++] = static_cast<uint8_t>(bit_count >> shift);
        }
        Transform();
        block_used_ = 0;
        for (size_t i = 0; i < state_.size(); ++i) {
            digest_[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
            digest_[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest_[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest_[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        finalized_ = true;
        return digest_;
    }

    constexpr bool IsValid() const { return !invalid_; }

    static constexpr std::array<char, 65> Hex(const Digest& digest) {
        constexpr char digits[] = "0123456789ABCDEF";
        std::array<char, 65> out{};
        for (size_t i = 0; i < digest.size(); ++i) {
            out[i * 2] = digits[digest[i] >> 4];
            out[i * 2 + 1] = digits[digest[i] & 15];
        }
        return out;
    }

private:
    static constexpr uint32_t RotateRight(uint32_t value, unsigned bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    constexpr void Transform() {
        constexpr std::array<uint32_t, 64> rounds{
            0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
            0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
            0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
            0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
            0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
            0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
            0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
            0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
            0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
            0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
            0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
            0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
            0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
            0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
            0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
            0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
        };
        std::array<uint32_t, 64> words{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t at = i * 4;
            words[i] = (uint32_t{block_[at]} << 24) |
                       (uint32_t{block_[at + 1]} << 16) |
                       (uint32_t{block_[at + 2]} << 8) |
                       uint32_t{block_[at + 3]};
        }
        for (size_t i = 16; i < words.size(); ++i) {
            const uint32_t s0 = RotateRight(words[i - 15], 7) ^
                                RotateRight(words[i - 15], 18) ^
                                (words[i - 15] >> 3);
            const uint32_t s1 = RotateRight(words[i - 2], 17) ^
                                RotateRight(words[i - 2], 19) ^
                                (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (size_t i = 0; i < words.size(); ++i) {
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t upper = RotateRight(e, 6) ^ RotateRight(e, 11) ^
                                   RotateRight(e, 25);
            const uint32_t lower = RotateRight(a, 2) ^ RotateRight(a, 13) ^
                                   RotateRight(a, 22);
            const uint32_t t1 = h + upper + choice + rounds[i] + words[i];
            const uint32_t t2 = lower + majority;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<uint32_t, 8> state_{
        0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
        0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
    std::array<uint8_t, 64> block_{};
    size_t block_used_ = 0;
    uint64_t total_bytes_ = 0;
    bool finalized_ = false;
    bool invalid_ = false;
    Digest digest_{};
};

// SHA-256 block function on the ARMv8 Cryptography Extension. Every ARM64
// device this port supports reports it; the portable rounds stay the fallback.
// About ten times faster than the portable code: the startup checks hash the
// 49.9 MB guest ELF, the stage cache hashes every binary it imports.
#include "obb_sha256.h"

#include <arm_neon.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>

namespace {

constexpr uint32_t kRounds[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

__attribute__((target("sha2")))
void Blocks(uint32_t* state, const uint8_t* data, size_t count) {
    uint32x4_t abcd = vld1q_u32(state);
    uint32x4_t efgh = vld1q_u32(state + 4);
    while (count--) {
        const uint32x4_t abcd_start = abcd, efgh_start = efgh;
        // Message words are big-endian; each vector holds four of them.
        uint32x4_t words[4] = {
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data))),
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16))),
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 32))),
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 48))),
        };
        for (unsigned group = 0; group < 16; ++group) {
            uint32x4_t& current = words[group & 3];
            const uint32x4_t schedule = vaddq_u32(current, vld1q_u32(kRounds + group * 4));
            const uint32x4_t abcd_before = abcd;
            abcd = vsha256hq_u32(abcd, efgh, schedule);
            efgh = vsha256h2q_u32(efgh, abcd_before, schedule);
            // W[t+16..t+19] from W[t..t+15], replacing the four just consumed.
            if (group < 12) {
                current = vsha256su1q_u32(vsha256su0q_u32(current, words[(group + 1) & 3]),
                                          words[(group + 2) & 3], words[(group + 3) & 3]);
            }
        }
        abcd = vaddq_u32(abcd, abcd_start);
        efgh = vaddq_u32(efgh, efgh_start);
        data += 64;
    }
    vst1q_u32(state, abcd);
    vst1q_u32(state + 4, efgh);
}

bool Available() {
    static const bool available = (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
    return available;
}

}  // namespace

bool Bl2Arm64Sha256Blocks(uint32_t* state, const uint8_t* blocks, size_t count) {
    if (!Available()) return false;
    Blocks(state, blocks, count);
    return true;
}

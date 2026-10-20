#include <arm_neon.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <cstdint>
#include <cstring>
#include "obb_sha256.h"

namespace {
const bool supportsSha2 = (getauxval(AT_HWCAP2) & HWCAP2_SHA2) != 0;
// Only this function can emit SHA2 instructions. The ARMv7 dispatcher checks
// the kernel's AArch32 HWCAP2 bits before entering it.
__attribute__((target("arch=cortex-a53,sha2"),noinline))
void transformSha2(std::uint32_t* state, const std::uint8_t* block,
                   const std::uint32_t* rounds) {
    uint32x4_t savedAbcd, savedEfgh;
    __builtin_memcpy(&savedAbcd,state,16);
    __builtin_memcpy(&savedEfgh,state + 4,16);
    auto abcd = savedAbcd, efgh = savedEfgh;
    uint32x4_t words[4];
#pragma clang loop unroll(full)
    for (unsigned i = 0; i < 4; ++i) {
        uint8x16_t bytes;
        __builtin_memcpy(&bytes,block + i*16,16);
        words[i] = (uint32x4_t)__builtin_shufflevector(bytes,bytes,
            3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
    }
#pragma clang loop unroll(full)
    for (unsigned group = 0; group < 16; ++group) {
        const auto i = group & 3;
        if (group >= 4) {
            // NDK arm_neon.h hides the named crypto intrinsics behind
            // __ARM_ARCH >= 8; its exact builtins work in this target function.
            const auto partial = (uint32x4_t)__builtin_neon_vsha256su0q_u32(
                (int8x16_t)words[i],(int8x16_t)words[(i+1)&3],50);
            words[i] = (uint32x4_t)__builtin_neon_vsha256su1q_u32(
                (int8x16_t)partial,(int8x16_t)words[(i+2)&3],(int8x16_t)words[(i+3)&3],50);
        }
        uint32x4_t constants;
        __builtin_memcpy(&constants,rounds + group*4,16);
        const auto wk = words[i] + constants;
        const auto previousAbcd = abcd;
        abcd = (uint32x4_t)__builtin_neon_vsha256hq_u32(
            (int8x16_t)previousAbcd,(int8x16_t)efgh,(int8x16_t)wk,50);
        efgh = (uint32x4_t)__builtin_neon_vsha256h2q_u32(
            (int8x16_t)efgh,(int8x16_t)previousAbcd,(int8x16_t)wk,50);
    }
    abcd += savedAbcd;
    efgh += savedEfgh;
    __builtin_memcpy(state,&abcd,16);
    __builtin_memcpy(state + 4,&efgh,16);
}
}

bool Bl2NativeSha256Transform(std::uint32_t* state, const std::uint8_t* block,
                             const std::uint32_t* rounds) {
    if (!supportsSha2) return false;
    transformSha2(state,block,rounds);
    return true;
}

bool NativeSha256SelfCheck() {
    const auto matches = [](ObbSha256& hash, const char* expected) {
        const auto hex = ObbSha256::Hex(hash.Finalize());
        return hash.IsValid() && std::strcmp(hex.data(),expected) == 0;
    };
    ObbSha256 empty, abc, twoBlocks, million;
    const char* longMessage = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    std::uint8_t chunk[1000];
    std::memset(chunk,'a',sizeof(chunk));
    if (!abc.Update(reinterpret_cast<const std::uint8_t*>("abc"),3) ||
        !twoBlocks.Update(reinterpret_cast<const std::uint8_t*>(longMessage),std::strlen(longMessage))) return false;
    for (unsigned i = 0; i < 1000; ++i) if (!million.Update(chunk,sizeof(chunk))) return false;
    return matches(empty,"E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855") &&
        matches(abc,"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") &&
        matches(twoBlocks,"248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1") &&
        matches(million,"CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0");
}

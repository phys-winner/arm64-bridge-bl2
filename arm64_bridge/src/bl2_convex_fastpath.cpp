#include "bl2_convex_fastpath.h"

#include "bridge_log.h"
#include "dynarmic_host.h"
#include "thunk_manager.h"

#include <arm_neon.h>
#include <dynarmic/interface/A32/a32.h>
#include <sys/system_properties.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#define LOG_TAG "Arm32Bridge_ConvexFast"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

namespace {

constexpr char kSymbol[] = "_ZNK13FConvexVolume12IntersectBoxERK7FVectorS2_";
constexpr char kOutcodeSymbol[] =
    "_ZNK13FConvexVolume25GetBoxIntersectionOutcodeERK7FVectorS2_";
constexpr uint32_t kOriginalPush = 0xE92D08F8u;
constexpr uint32_t kOriginalVpush = 0xED2D8B04u;
constexpr uint32_t kOutcodeVpush = 0xED2D8B0Cu;
constexpr uint32_t kArmLdrPcLiteral = 0xE51FF004u;
constexpr size_t kObjectSize = 0x300;
constexpr size_t kPlanesSize = 0x400;

bool Valid(const DynarmicHost& host, uint32_t address, size_t size) {
    return address && static_cast<uint64_t>(address) + size <=
        host.GetGuestMemorySize();
}

template <typename T>
T Read(const uint8_t* address) {
    T value;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

template <typename T>
void Write(uint8_t* address, T value) {
    std::memcpy(address, &value, sizeof(value));
}

#pragma clang fp contract(off)
std::optional<uint32_t> NativeIntersectBox(
    DynarmicHost& host, uint32_t object_address,
    uint32_t center_address, uint32_t extent_address) {
    if (!Valid(host, object_address, 0x114) ||
        !Valid(host, center_address, 12) ||
        !Valid(host, extent_address, 12)) return std::nullopt;
    const uint8_t* const memory = host.GetGuestMemoryBase();
    const uint8_t* const object = memory + object_address;
    const int32_t plane_count = Read<int32_t>(object + 0x110);
    if (plane_count <= 0) return 1u;
    if (plane_count > 256) return std::nullopt;
    const uint32_t external = Read<uint32_t>(object + 0x100);
    const uint32_t plane_address = external ? external : object_address + 0x80;
    const size_t groups = (static_cast<size_t>(plane_count) + 3) / 4;
    if (!Valid(host, plane_address, groups * 64) || (plane_address & 3)) {
        return std::nullopt;
    }
    const uint8_t* const center = memory + center_address;
    const uint8_t* const extent = memory + extent_address;
    const float cx = Read<float>(center), cy = Read<float>(center + 4),
        cz = Read<float>(center + 8);
    const float ex = Read<float>(extent), ey = Read<float>(extent + 4),
        ez = Read<float>(extent + 8);
    uint32x4_t outside = vdupq_n_u32(0);
    for (size_t group = 0; group < groups; ++group) {
        const float* const planes = reinterpret_cast<const float*>(
            memory + plane_address + group * 64);
        const float32x4_t x = vld1q_f32(planes);
        const float32x4_t y = vld1q_f32(planes + 4);
        const float32x4_t z = vld1q_f32(planes + 8);
        const float32x4_t w = vld1q_f32(planes + 12);
        float32x4_t distance = vmulq_n_f32(x, cx);
        distance = vaddq_f32(distance, vmulq_n_f32(y, cy));
        distance = vaddq_f32(distance, vmulq_n_f32(z, cz));
        distance = vsubq_f32(distance, w);
        float32x4_t radius = vmulq_n_f32(vabsq_f32(x), ex);
        radius = vaddq_f32(radius, vmulq_n_f32(vabsq_f32(y), ey));
        radius = vaddq_f32(radius, vmulq_n_f32(vabsq_f32(z), ez));
        outside = vorrq_u32(outside, vcgtq_f32(distance, radius));
    }
    return vmaxvq_u32(outside) ? 0u : 1u;
}

// ARM32 returns bit 0 when no plane is beyond +radius, and bit 1 when any
// plane is beyond -radius. Keep its strict comparisons and operation order.
std::optional<uint32_t> NativeOutcode(
    DynarmicHost& host, uint32_t object_address,
    uint32_t center_address, uint32_t extent_address) {
    if (!Valid(host, object_address, 0x114) ||
        !Valid(host, center_address, 12) ||
        !Valid(host, extent_address, 12)) return std::nullopt;
    const uint8_t* const memory = host.GetGuestMemoryBase();
    const uint8_t* const object = memory + object_address;
    const int32_t plane_count = Read<int32_t>(object + 0x110);
    if (plane_count <= 0) return 1u;
    if (plane_count > 256) return std::nullopt;
    const uint32_t external = Read<uint32_t>(object + 0x100);
    const uint32_t plane_address = external ? external : object_address + 0x80;
    const size_t groups = (static_cast<size_t>(plane_count) + 3) / 4;
    if (!Valid(host, plane_address, groups * 64) || (plane_address & 3)) {
        return std::nullopt;
    }
    const uint8_t* const center = memory + center_address;
    const uint8_t* const extent = memory + extent_address;
    const float cx = Read<float>(center), cy = Read<float>(center + 4),
        cz = Read<float>(center + 8);
    const float ex = Read<float>(extent), ey = Read<float>(extent + 4),
        ez = Read<float>(extent + 8);
    uint32x4_t beyond_positive = vdupq_n_u32(0);
    uint32x4_t beyond_negative = vdupq_n_u32(0);
    for (size_t group = 0; group < groups; ++group) {
        const float* const planes = reinterpret_cast<const float*>(
            memory + plane_address + group * 64);
        const float32x4_t x = vld1q_f32(planes);
        const float32x4_t y = vld1q_f32(planes + 4);
        const float32x4_t z = vld1q_f32(planes + 8);
        const float32x4_t w = vld1q_f32(planes + 12);
        float32x4_t distance = vmulq_n_f32(x, cx);
        distance = vaddq_f32(distance, vmulq_n_f32(y, cy));
        distance = vaddq_f32(distance, vmulq_n_f32(z, cz));
        distance = vsubq_f32(distance, w);
        float32x4_t radius = vmulq_n_f32(vabsq_f32(x), ex);
        radius = vaddq_f32(radius, vmulq_n_f32(vabsq_f32(y), ey));
        radius = vaddq_f32(radius, vmulq_n_f32(vabsq_f32(z), ez));
        beyond_positive = vorrq_u32(beyond_positive,
                                     vcgtq_f32(distance, radius));
        beyond_negative = vorrq_u32(beyond_negative,
                                     vcgtq_f32(distance, vnegq_f32(radius)));
    }
    return (vmaxvq_u32(beyond_positive) ? 0u : 1u) |
           (vmaxvq_u32(beyond_negative) ? 2u : 0u);
}

uint32_t NextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float NextFloat(uint32_t& state) {
    return static_cast<float>(static_cast<int32_t>(NextRandom(state) % 20001) - 10000) / 1000.0f;
}

bool DifferentialSelfTest(DynarmicHost& host, uint32_t function,
                          uint32_t object, uint32_t planes,
                          uint32_t center, uint32_t extent,
                          bool patched, bool outcode) {
    uint8_t* const memory = host.GetGuestMemoryBase();
    uint32_t random = 0xC0FFEE21u;
    for (uint32_t test = 0; test < 512; ++test) {
        std::memset(memory + object, 0, kObjectSize);
        const bool inline_planes = test % 4 == 0;
        const int32_t count = inline_planes
            ? static_cast<int32_t>(test % 9)
            : static_cast<int32_t>(test % 17);
        const uint32_t location = inline_planes ? object + 0x80 : planes;
        Write<uint32_t>(memory + object + 0x100,
                        inline_planes ? 0u : planes);
        Write<int32_t>(memory + object + 0x110, count);
        for (size_t lane = 0; lane < 3; ++lane) {
            Write<float>(memory + center + lane * 4, NextFloat(random));
            Write<float>(memory + extent + lane * 4,
                         static_cast<float>(NextRandom(random) % 4001) / 1000.0f);
        }
        const size_t groups = (static_cast<size_t>(count) + 3) / 4;
        for (size_t word = 0; word < groups * 16; ++word) {
            Write<float>(memory + location + word * 4, NextFloat(random));
        }
        if (groups && test % 8 == 1) {
            for (size_t group = 0; group < groups; ++group) {
                for (size_t lane = 0; lane < 4; ++lane) {
                    Write<float>(memory + location + group * 64 + lane * 4, 0.0f);
                    Write<float>(memory + location + group * 64 + 16 + lane * 4, 0.0f);
                    Write<float>(memory + location + group * 64 + 32 + lane * 4, 0.0f);
                    Write<float>(memory + location + group * 64 + 48 + lane * 4, 1000.0f);
                }
            }
            Write<float>(memory + location, 1.0f);
            const float edge = Read<float>(memory + center) -
                               Read<float>(memory + extent);
            Write<float>(memory + location + 48,
                         (test & 16) ? std::nextafter(edge, -INFINITY) : edge);
        }
        const auto native = outcode
            ? NativeOutcode(host, object, center, extent)
            : NativeIntersectBox(host, object, center, extent);
        if (!native) return false;
        const uint32_t guest = host.RunFunction(
            function, {object, center, extent});
        if (guest != *native) {
            LOGE("Convex differential %u failed: guest=%u arm64=%u count=%d "
                 "inline=%d patched=%d outcode=%d", test, guest, *native,
                 count, inline_planes, patched, outcode);
            return false;
        }
    }
    LOGI("Convex differential passed: cases=512 patched=%d outcode=%d",
         patched, outcode);
    return true;
}

}  // namespace

bool InstallBl2ConvexFastpath(DynarmicHost& host) {
    char property[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2.convex_fastpath", property) > 0 &&
        property[0] == '0' && property[1] == '\0') return true;
    const uint32_t function = host.FindSymbol(kSymbol);
    const uint32_t outcode_function = host.FindSymbol(kOutcodeSymbol);
    if (!Valid(host, function, 8) ||
        host.MemoryRead32(function) != kOriginalPush ||
        host.MemoryRead32(function + 4) != kOriginalVpush ||
        !Valid(host, outcode_function, 8) ||
        host.MemoryRead32(outcode_function) != kOriginalPush ||
        host.MemoryRead32(outcode_function + 4) != kOutcodeVpush) {
        LOGE("Convex fast path refused: guest signature mismatch at "
             "0x%08X/0x%08X", function, outcode_function);
        return false;
    }
    const uint32_t object = host.GuestAlloc(kObjectSize, 16);
    const uint32_t planes = host.GuestAlloc(kPlanesSize, 16);
    const uint32_t center = host.GuestAlloc(16, 16);
    const uint32_t extent = host.GuestAlloc(16, 16);
    const uint32_t fallback = host.GuestAlloc(16, 4);
    const uint32_t outcode_fallback = host.GuestAlloc(16, 4);
    const auto cleanup = [&] {
        if (outcode_fallback) host.GuestFree(outcode_fallback);
        if (fallback) host.GuestFree(fallback);
        if (extent) host.GuestFree(extent);
        if (center) host.GuestFree(center);
        if (planes) host.GuestFree(planes);
        if (object) host.GuestFree(object);
    };
    if (!object || !planes || !center || !extent || !fallback ||
        !outcode_fallback ||
        !DifferentialSelfTest(host, function, object, planes, center, extent,
                              false, false) ||
        !DifferentialSelfTest(host, outcode_function, object, planes,
                              center, extent, false, true)) {
        LOGE("Convex fast path refused: original differential failed");
        cleanup();
        return false;
    }
    host.MemoryWrite32(fallback, kOriginalPush);
    host.MemoryWrite32(fallback + 4, kOriginalVpush);
    host.MemoryWrite32(fallback + 8, kArmLdrPcLiteral);
    host.MemoryWrite32(fallback + 12, function + 8);
    host.InvalidateGuestCode(fallback, 16);
    host.MemoryWrite32(outcode_fallback, kOriginalPush);
    host.MemoryWrite32(outcode_fallback + 4, kOutcodeVpush);
    host.MemoryWrite32(outcode_fallback + 8, kArmLdrPcLiteral);
    host.MemoryWrite32(outcode_fallback + 12, outcode_function + 8);
    host.InvalidateGuestCode(outcode_fallback, 16);
    const uint32_t thunk = host.GetThunkManager().RegisterThunk(
        "bl2_convex_fastpath",
        [fallback](Dynarmic::A32::Jit& jit, DynarmicHost& thunk_host) {
            const auto result = NativeIntersectBox(thunk_host, jit.Regs()[0],
                                                   jit.Regs()[1], jit.Regs()[2]);
            if (result) {
                jit.Regs()[0] = *result;
            } else {
                jit.Regs()[15] = fallback;
            }
        });
    if (!thunk) {
        cleanup();
        return false;
    }
    const uint32_t outcode_thunk = host.GetThunkManager().RegisterThunk(
        "bl2_convex_outcode_fastpath",
        [outcode_fallback](Dynarmic::A32::Jit& jit, DynarmicHost& thunk_host) {
            const auto result = NativeOutcode(thunk_host, jit.Regs()[0],
                                              jit.Regs()[1], jit.Regs()[2]);
            if (result) {
                jit.Regs()[0] = *result;
            } else {
                jit.Regs()[15] = outcode_fallback;
            }
        });
    if (!outcode_thunk) {
        cleanup();
        return false;
    }
    host.MemoryWrite32(function, kArmLdrPcLiteral);
    host.MemoryWrite32(function + 4, thunk);
    host.InvalidateGuestCode(function, 8);
    host.MemoryWrite32(outcode_function, kArmLdrPcLiteral);
    host.MemoryWrite32(outcode_function + 4, outcode_thunk);
    host.InvalidateGuestCode(outcode_function, 8);
    if (!DifferentialSelfTest(host, function, object, planes, center, extent,
                              true, false) ||
        !DifferentialSelfTest(host, outcode_function, object, planes,
                              center, extent, true, true)) {
        host.MemoryWrite32(function, kOriginalPush);
        host.MemoryWrite32(function + 4, kOriginalVpush);
        host.InvalidateGuestCode(function, 8);
        host.MemoryWrite32(outcode_function, kOriginalPush);
        host.MemoryWrite32(outcode_function + 4, kOutcodeVpush);
        host.InvalidateGuestCode(outcode_function, 8);
        LOGE("Convex fast path refused: patched differential failed; original restored");
        cleanup();
        return false;
    }
    host.GuestFree(extent);
    host.GuestFree(center);
    host.GuestFree(planes);
    host.GuestFree(object);
    LOGI("Convex fast path active: arm64-neon box=0x%08X outcode=0x%08X "
         "thunks=0x%08X/0x%08X", function, outcode_function,
         thunk, outcode_thunk);
    return true;
}

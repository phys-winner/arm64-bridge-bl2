#pragma once
#include <cstddef>
#include <cstdint>

// ARM32-only loaded-code contract. VerifyEngine already admits only this ELF;
// the mapped PIC instructions and resolved GOT bindings are checked again.
namespace bl2::native_render {
inline constexpr char kEngineSha256[] =
    "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814";
inline constexpr char kSymbol[] = "_Z21NativeCallback_RenderPv";
inline constexpr std::uint32_t kImageBytes = 0x03089000;
inline constexpr std::uint32_t kFunctionRva = 0x01E25378;
inline constexpr std::uint32_t kFunctionBytes = 0x000005B0;
struct Word { std::uint32_t rva, value; };
inline constexpr Word kGuards[]{
    {0x01E25378,0xE92D4FF0},
    {0x01E2537C,0xE24DD054},
    {0x01E2538C,0xE59F5448},
    {0x01E253A0,0xE08F5005},
    {0x01E254D4,0xE59F1330},
    {0x01E254D8,0xE59F0330},
    {0x01E254DC,0xE59F2330},
    {0x01E254E0,0xE59F3330},
    {0x01E254E4,0xE7951001},
    {0x01E254E8,0xE7958000},
    {0x01E254EC,0xE59FB328},
    {0x01E254F0,0xE59FC328},
    {0x01E254F4,0xE58D1024},
    {0x01E254F8,0xE7952002},
    {0x01E254FC,0xE59F7320},
    {0x01E25500,0xE59F4320},
    {0x01E25504,0xE08FB00B},
    {0x01E25508,0xE08FC00C},
    {0x01E2550C,0xE58D2020},
    {0x01E25510,0xE7953003},
    {0x01E25514,0xE08F7007},
    {0x01E25518,0xE58DC028},
    {0x01E2551C,0xE08F4004},
    {0x01E25520,0xE58D301C},
    {0x01E256C4,0xE12FFF38},
    {0x01E256C8,0xE59DC024},
    {0x01E256CC,0xE12FFF3C},
    {0x01E256D0,0xE59DC020},
    {0x01E256D4,0xE12FFF3C},
    {0x01E256D8,0xE3500011},
    {0x01E256DC,0xE1A02000},
    {0x01E256E0,0x0A000089},
    {0x01E25720,0xE59DC01C},
    {0x01E25724,0xE12FFF3C},
    {0x01E25728,0xE59DC018},
    {0x01E2578C,0xE1A00004},
    {0x01E25790,0xE28DD054},
    {0x01E25794,0xE8BD8FF0},
    {0x01E257DC,0x01114958},
    {0x01E2580C,0xFFFF2D70},
    {0x01E25810,0xFFFF3078},
    {0x01E25814,0xFFFF2D78},
    {0x01E25818,0xFFFF2D74},
};
struct Binding { std::uint32_t slotRva, targetRva; const char* symbol; };
inline constexpr Binding kBindings[]{
    {0x02F2CA70,0x02BA81F0,"nvsiLock"},
    {0x02F2CA78,0x02BA7578,"nvsiCheckStatus"},
    {0x02F2CA74,0x02BA8218,"nvsiUnlock"},
};
inline constexpr Word kPatches[]{
    {0x01E256CC,0xE1A00000}, // No render-owned NVSI mutex acquisition.
    {0x01E256D4,0xEA000011}, // B 01E25720: skip only NVSI status/error routing.
    {0x01E25724,0xE1A00000}, // No render-owned NVSI mutex release.
};

// Validate every instruction/literal/binding before any write. The caller
// rejects initialization if a write/cache operation fails its readback.
template<class Read, class Write, class Flush>
bool Apply(std::uint32_t base, Read read, Write write, Flush flush) {
    if (base > UINT32_MAX-kImageBytes) return false;
    for (const auto& word:kGuards)
        if (read(base+word.rva)!=word.value) return false;
    for (const auto& binding:kBindings)
        if (read(base+binding.slotRva)!=base+binding.targetRva) return false;
    for (const auto& word:kPatches) write(base+word.rva,word.value);
    flush(base+kFunctionRva,kFunctionBytes);
    for (const auto& word:kPatches)
        if (read(base+word.rva)!=word.value) return false;
    return true;
}
} // namespace bl2::native_render

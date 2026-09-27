#include "bl2_touch_gameplay_hooks.h"
#include "bl2_touch_gameplay_sink.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#include <array>
#include <cstdint>

namespace {
bl2::touch::Ue3GameplaySink* activeSink = nullptr;
struct Signature { const char* name; std::uint32_t rva; std::array<std::uint32_t, 4> prefix; };
constexpr Signature signatures[] = {
    {"_ZN12UPlayerInput14PIProcessInputEf", 0x0183AA48,
        {0xEE071A10,0xEDDF7A18,0xEDD06A40,0xED906A45}},
    {"_ZN6UInput15UpdateAxisValueEPff", 0x01821D6C,
        {0xEE072A10,0xEDD17A00,0xEE777A87,0xEDC17A00}},
    {"_ZN18UWillowPlayerInput16GetTargetAxisPtrE14EAxisParameter", 0x0236E210,
        {0xE92D4070,0xE3A03000,0xE59F4298,0xE24DD018}},
    {"_ZN18UWillowPlayerInput17IsActionAvailableEP22UInputActionDefinition", 0x0235679C,
        {0xE92D0070,0xE5903290,0xE3530000,0x1A000002}},
    {"_ZN18UWillowPlayerInput24FireInputActionBehaviorsERK6TArrayIP22UInputActionDefinition21TAlignedHeapAllocatorILm8EEE17EInputActionEventjj",
        0x0236D8FC, {0xE92D47F0,0xE1A04001,0xE5911004,0xE24DD018}},
};
// PIProcessInput's +4 VLDR is PC relative, so leave its first 8 bytes intact.
constexpr std::uint32_t movementRva = 0x0183AA50;
constexpr std::uint32_t lookRva = 0x0183ABCC;
constexpr std::array<std::uint32_t, 4> lookPreimage = {0xE59530E0,0xE3530000,0x0A00001E,0xE593236C};
void movement(Dynarmic::A32::Jit& jit, DynarmicHost&) {
    if (activeSink) activeSink->beforeAxisProcessing(jit.Regs()[0]);
}
void look(Dynarmic::A32::Jit& jit, DynarmicHost&) {
    if (activeSink) activeSink->beforeLookClamp(jit.Regs()[4]);
}
} // namespace

bool InstallBl2TouchGameplayHooks(DynarmicHost& host, bl2::touch::Ue3GameplaySink* sink) {
    if (!sink) return false;
    if (activeSink) return activeSink == sink;
    const auto& elf = host.GetElfLoader();
    const auto base = elf.GetBaseAddress();
    if (elf.GetImageSize() != 0x03089000 ||
        std::uint64_t(base) + elf.GetImageSize() > host.GetGuestMemorySize()) return false;
    for (const auto& signature : signatures) {
        if (elf.GetSymbolAddress(signature.name) != base + signature.rva) return false;
        for (std::size_t i = 0; i != signature.prefix.size(); ++i)
            if (host.MemoryRead32(base + signature.rva + std::uint32_t(i * 4)) != signature.prefix[i]) return false;
    }
    if (elf.GetSymbolAddress("_ZN12UPlayerInput17PIPostAdjustInputEff") != base + 0x0183AAB8) return false;
    for (std::size_t i = 0; i != lookPreimage.size(); ++i)
        if (host.MemoryRead32(base + lookRva + std::uint32_t(i * 4)) != lookPreimage[i]) return false;
    const auto code = host.GuestAlloc(40, 4);
    if (!code) return false;
    const std::array<std::uint32_t, 2> sites = {base + movementRva, base + lookRva};
    const std::array<const char*, 2> names = {"bl2_touch_native_move_axis", "bl2_touch_native_look_delta"};
    const std::array<ThunkHandler, 2> handlers = {movement, look};
    for (std::size_t i = 0; i != sites.size(); ++i) {
        const auto thunk = host.GetThunkManager().RegisterThunk(names[i], handlers[i]);
        if (!thunk) return false;
        const auto svc = host.MemoryRead32(thunk);
        if ((svc & 0xFF000000) != 0xEF000000) return false;
        const auto veneer = code + std::uint32_t(i * 20);
        host.MemoryWrite32(veneer, svc);
        host.MemoryWrite32(veneer + 4, host.MemoryRead32(sites[i]));
        host.MemoryWrite32(veneer + 8, host.MemoryRead32(sites[i] + 4));
        host.MemoryWrite32(veneer + 12, 0xE51FF004);
        host.MemoryWrite32(veneer + 16, sites[i] + 8);
    }
    host.AddExecutableRange(code, 40); host.InvalidateGuestCode(code, 40);
    for (std::size_t i = 0; i != sites.size(); ++i) {
        host.MemoryWrite32(sites[i], 0xE51FF004);
        host.MemoryWrite32(sites[i] + 4, code + std::uint32_t(i * 20));
        host.InvalidateGuestCode(sites[i], 8);
    }
    activeSink = sink;
    HORN_LOGI("Arm32Bridge_Touch", "native-touch gameplay axis hooks installed; movement=PIProcessInput+8 look=PIPostAdjustInput+0x114");
    return true;
}

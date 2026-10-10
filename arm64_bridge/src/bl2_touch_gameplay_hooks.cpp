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
    {"_ZN23AWillowPlayerController11GetHUDMovieEv", 0x0235734C,
        {0xE92D4070,0xE59056E0,0xE59F4090,0xE3550000}},
    {"_ZN23AWillowInventoryManager19CountReadiedWeaponsEv", 0x0229ECA0,
        {0xE92D4FF8,0xE5905188,0xE59F91B0,0xE3550000}},
    {"_ZN7UObject12ProcessEventEP9UFunctionPvS2_", 0x010E826C,
        {0xE92D4FF0,0xED2D8B06,0xE1A04001,0xE3003402}},
    {"_ZN6AActor12ProcessEventEP9UFunctionPvS2_", 0x010E86C8,
        {0xE59FC088,0xE92D41F0,0xE1A05000,0xE1A08001}},
    {"_ZN13UResourcePool15GetCurrentValueEv", 0x01ABCA00,
        {0xE590006C,0xE12FFF1E,0xE92D4010,0xEDD07A26}},
    {"_Z7appFreePv", 0x00F26444,
        {0xE92D4010,0xE1A01000,0xE59F4044,0xE24DD008}},
    {"_ZN16UPlayerSkillTree14GetActionSkillEv", 0x0207B904,
        {0xE59030A4,0xE3530000,0xBA000009,0xE590205C}},
    {"_ZN11AWillowPawn20IsActionSkillRunningEv", 0x0233735C,
        {0xE5900CB4,0xE2900000,0x13A00001,0xE12FFF1E}},
};
// Passive HUD lifetime fields are bound to the original setters/readers, not
// inferred from the interactive GFx menu contract. These interior words are
// outside the existing StartScene/CloseScene entry veneers.
struct FieldGuard { std::uint32_t rva; std::array<std::uint32_t,4> words; };
constexpr FieldGuard hudFieldGuards[] = {
    {0x01CC52A8,{0xE590303C,0xE3530000,0x1583105C,0xE12FFF1E}},
    {0x01CD8760,{0xE581A048,0xE59DC068,0xE08F8008,0xE581C050}},
    {0x01CDEF68,{0xE5932048,0xE3520000,0x1A00000D,0xE59F607C}},
    {0x01CD16B4,{0xE5841048,0xE5982010,0xE5985014,0xE0825105}},
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
    for (const auto& guard:hudFieldGuards)
        for (std::size_t i=0;i!=guard.words.size();++i)
            if (host.MemoryRead32(base+guard.rva+std::uint32_t(i*4))!=guard.words[i]) return false;
    if (elf.GetSymbolAddress("_ZN12UPlayerInput17PIPostAdjustInputEff") != base + 0x0183AAB8) return false;
    for (std::size_t i = 0; i != lookPreimage.size(); ++i)
        if (host.MemoryRead32(base + lookRva + std::uint32_t(i * 4)) != lookPreimage[i]) return false;
    const std::array<std::uint32_t, 2> sites = {base + movementRva, base + lookRva};
    const std::array<const char*, 2> names = {"bl2_touch_native_move_axis", "bl2_touch_native_look_delta"};
    const std::array<ThunkHandler, 2> handlers = {movement, look};
#if defined(BL2_NATIVE_ARM32)
    std::array<std::uint32_t, 2> dispatchers{};
    for (std::size_t i = 0; i < sites.size(); ++i) {
        const std::array<std::uint32_t, 4> words = {
            host.MemoryRead32(sites[i]), host.MemoryRead32(sites[i] + 4),
            0xE51FF004, sites[i] + 8};
        const auto continuation = host.CreateCodeVeneer(words);
        if (!continuation) return false;
        const auto handler = handlers[i];
        dispatchers[i] = host.GetThunkManager().RegisterThunk(names[i],
            [handler,continuation](Dynarmic::A32::Jit& jit, DynarmicHost& current) {
                handler(jit,current);
                jit.Regs()[15] = continuation;
            });
        if (!dispatchers[i]) return false;
    }
    for (std::size_t i = 0; i < sites.size(); ++i) {
        host.MemoryWrite32(sites[i], 0xE51FF004);
        host.MemoryWrite32(sites[i] + 4, dispatchers[i]);
        host.InvalidateGuestCode(sites[i], 8);
    }
#else
    const auto code = host.GuestAlloc(40, 4);
    if (!code) return false;
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
#endif
    activeSink = sink;
    HORN_LOGI("Arm32Bridge_Touch", "native-touch gameplay axis hooks installed; movement=PIProcessInput+8 look=PIPostAdjustInput+0x114");
    return true;
}

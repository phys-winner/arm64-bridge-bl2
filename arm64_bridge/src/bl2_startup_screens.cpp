#include "bl2_startup_screens.h"

#if !defined(BL2_STARTUP_SCREENS_HOST_TEST)
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#endif
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>

namespace {
constexpr std::uint32_t kGetField = 0x01871D10;
constexpr std::uint32_t kStringAssign = 0x00F27A68;
constexpr std::uint32_t kGetFieldWords[] = {
    0xE92D4070, 0xE3A0C000, 0xE24DD028, 0xE1A00001};
constexpr std::uint32_t kAssignWords[] = {
    0xE92D4070, 0xE1A05001, 0xE5901000, 0xE24DD008};

bool Span(const DynarmicHost& host, std::uint32_t address, std::size_t bytes) {
    return address >= 0x10000 && address <= host.GetGuestMemorySize() &&
        bytes <= host.GetGuestMemorySize() - address;
}

bool StartupFlag(DynarmicHost& host, std::uint32_t string) {
    if (!Span(host, string, 12) || (string & 3)) return false;
    const auto data = host.MemoryRead32(string);
    const auto count = host.MemoryRead32(string + 4);
    const auto capacity = host.MemoryRead32(string + 8);
    if (count > capacity || capacity > 4096 || !Span(host, data, count)) return false;
    for (const std::string_view key : {"<Registry:SkipLegalScreen>", "<Registry:GoStraightToMainMenu>"})
        if (count == key.size() + 1 && host.MemoryRead8(data + count - 1) == 0 &&
            std::memcmp(host.GetGuestMemoryBase() + data, key.data(), key.size()) == 0) return true;
    return false;
}
} // namespace

bool InstallBl2StartupScreens(DynarmicHost& host) {
    const auto& elf = host.GetElfLoader();
    const auto base = elf.GetBaseAddress();
    if (elf.GetImageSize() != 0x03089000 ||
        std::uint64_t(base) + elf.GetImageSize() > host.GetGuestMemorySize() ||
        host.FindSymbol("_ZN7UUIRoot22GetDataStoreFieldValueERK7FStringR21FUIProviderFieldValueP12ULocalPlayer") != base + kGetField ||
        host.FindSymbol("_ZN7FStringaSEPKc") != base + kStringAssign ||
        !elf.IsCodeRange(base + kGetField, sizeof(kGetFieldWords)) ||
        !elf.IsCodeRange(base + kStringAssign, sizeof(kAssignWords))) return false;
    for (std::size_t i = 0; i < std::size(kGetFieldWords); ++i)
        if (host.MemoryRead32(base + kGetField + static_cast<std::uint32_t>(i * 4)) != kGetFieldWords[i] ||
            host.MemoryRead32(base + kStringAssign + static_cast<std::uint32_t>(i * 4)) != kAssignWords[i]) return false;
    const auto replay = host.GuestAlloc(44, 4);
    if (!replay) return false;
    const std::array<std::uint32_t, 11> words{
        kGetFieldWords[0], kGetFieldWords[1], 0xE51FF004, base + kGetField + 8,
        0xE92D4010, 0xE59FC008, 0xE12FFF3C, 0xE3A00001, 0xE8BD8010,
        base + kStringAssign, 0x31}; // Assign StringValue, return TRUE; "1" + NUL.
    for (std::size_t i = 0; i < words.size(); ++i)
        host.MemoryWrite32(replay + static_cast<std::uint32_t>(i * 4), words[i]);
    const auto thunk = host.GetThunkManager().RegisterThunk("bl2_startup_screens",
        [replay](Dynarmic::A32::Jit& jit, DynarmicHost& current) {
            auto& regs = jit.Regs();
            if (Span(current, regs[2], 24) && !(regs[2] & 3) && StartupFlag(current, regs[1])) {
                // Start/OnTick keep PendingLoginChange, SignIn and every save/session startup step.
                // UIRoot's script getter copies FUIProviderFieldValue.StringValue at +0x0C.
                regs[0] = regs[2] + 12;
                regs[1] = replay + 40;
                regs[15] = replay + 16;
            } else regs[15] = replay;
        });
    if (!thunk) { host.GuestFree(replay); return false; }
    host.AddExecutableRange(replay, 40);
    host.MemoryWrite32(base + kGetField, 0xE51FF004);
    host.MemoryWrite32(base + kGetField + 4, thunk);
    host.InvalidateGuestCode(replay, 40);
    host.InvalidateGuestCode(base + kGetField, 8);
    return true;
}

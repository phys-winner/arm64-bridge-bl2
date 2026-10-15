#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class DynarmicHost;
namespace Dynarmic::A32 {
struct Jit {
    std::array<std::uint32_t, 16> regs{};
    auto& Regs() { return regs; }
};
}
struct ThunkManager {
    std::function<void(Dynarmic::A32::Jit&, DynarmicHost&)> handler;
    bool fail = false;
    std::uint32_t RegisterThunk(const std::string&, decltype(handler) callback) {
        if (fail) return 0;
        handler = std::move(callback);
        return 0x10000;
    }
};
struct ElfLoader {
    std::uint32_t image = 0x03089000;
    bool executable = true;
    std::uint32_t GetBaseAddress() const { return 0x20000; }
    std::uint32_t GetImageSize() const { return image; }
    bool IsCodeRange(std::uint32_t, std::size_t) const { return executable; }
};
class DynarmicHost {
public:
    ElfLoader elf;
    ThunkManager thunks;
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(64 * 1024 * 1024);
    bool allocationFails = false, symbolFails = false, freed = false;
    std::vector<std::pair<std::uint32_t, std::size_t>> executable, invalidated;
    const ElfLoader& GetElfLoader() const { return elf; }
    ThunkManager& GetThunkManager() { return thunks; }
    std::size_t GetGuestMemorySize() const { return memory.size(); }
    std::uint8_t* GetGuestMemoryBase() { return memory.data(); }
    std::uint32_t MemoryRead32(std::uint32_t p) const {
        assert(p <= memory.size() - 4);
        std::uint32_t v; std::memcpy(&v, memory.data() + p, 4); return v;
    }
    std::uint8_t MemoryRead8(std::uint32_t p) const { return memory.at(p); }
    void MemoryWrite32(std::uint32_t p, std::uint32_t v) {
        assert(p <= memory.size() - 4); std::memcpy(memory.data() + p, &v, 4);
    }
    std::uint32_t FindSymbol(const std::string& s) const {
        if (symbolFails) return 0;
        return elf.GetBaseAddress() + (s == "_ZN7FStringaSEPKc" ? 0x00F27A68 : 0x01871D10);
    }
    std::uint32_t GuestAlloc(std::size_t n, std::size_t align) const {
        assert(n == 44 && align == 4); return allocationFails ? 0 : 0x3400000;
    }
    void GuestFree(std::uint32_t p) { assert(p == 0x3400000); freed = true; }
    void AddExecutableRange(std::uint32_t p, std::size_t n) { executable.emplace_back(p, n); }
    void InvalidateGuestCode(std::uint32_t p, std::size_t n) { invalidated.emplace_back(p, n); }
};

#define BL2_STARTUP_SCREENS_HOST_TEST
#include "../src/bl2_startup_screens.cpp"

int main() {
    DynarmicHost host;
    const auto base = host.elf.GetBaseAddress();
    const auto original = [&] {
        for (std::size_t i = 0; i < std::size(kGetFieldWords); ++i) {
            host.MemoryWrite32(base + kGetField + static_cast<std::uint32_t>(i * 4), kGetFieldWords[i]);
            host.MemoryWrite32(base + kStringAssign + static_cast<std::uint32_t>(i * 4), kAssignWords[i]);
        }
    };
    const auto refused = [&] {
        assert(!InstallBl2StartupScreens(host));
        assert(host.MemoryRead32(base + kGetField) == kGetFieldWords[0]);
        assert(host.executable.empty() && host.invalidated.empty() && !host.thunks.handler);
    };
    original(); host.elf.image = 0; refused(); host.elf.image = 0x03089000;
    host.symbolFails = true; refused(); host.symbolFails = false;
    host.elf.executable = false; refused(); host.elf.executable = true;
    for (const auto rva : {kGetField, kStringAssign}) {
        for (unsigned i = 0; i < 4; ++i) {
            host.MemoryWrite32(base + rva + i * 4, 0);
            assert(!InstallBl2StartupScreens(host)); original();
        }
    }
    host.allocationFails = true; refused(); host.allocationFails = false;
    host.thunks.fail = true; refused(); assert(host.freed); host.thunks.fail = false;
    assert(InstallBl2StartupScreens(host));
    assert(host.MemoryRead32(base + kGetField) == 0xE51FF004);
    assert(host.MemoryRead32(base + kGetField + 4) == 0x10000);
    constexpr std::uint32_t replay = 0x3400000, input = 0x3500000, data = input + 12;
    assert(host.MemoryRead32(replay) == kGetFieldWords[0]);
    assert(host.MemoryRead32(replay + 4) == kGetFieldWords[1]);
    assert(host.MemoryRead32(replay + 8) == 0xE51FF004);
    assert(host.MemoryRead32(replay + 12) == base + kGetField + 8);
    assert(host.MemoryRead32(replay + 16) == 0xE92D4010);
    assert(host.MemoryRead32(replay + 36) == base + kStringAssign);
    assert(host.MemoryRead32(replay + 40) == 0x31);
    assert(host.executable.size() == 1 && host.executable[0].second == 40);
    assert(host.invalidated.size() == 2);
    const auto string = [&](const char* key) {
        const auto n = static_cast<std::uint32_t>(std::strlen(key) + 1);
        std::memcpy(host.memory.data() + data, key, n);
        host.MemoryWrite32(input, data); host.MemoryWrite32(input + 4, n); host.MemoryWrite32(input + 8, n);
    };
    const auto dispatch = [&](bool skip, std::uint32_t arg = 0x3500000, std::uint32_t result = 0x3500100) {
        Dynarmic::A32::Jit jit;
        for (unsigned i = 0; i < 16; ++i) jit.regs[i] = 0x123000 + i * 4;
        jit.regs[1] = arg; jit.regs[2] = result;
        const auto before = jit.regs;
        host.thunks.handler(jit, host);
        assert(jit.regs[15] == (skip ? replay + 16 : replay));
        for (unsigned i = 0; i < 15; ++i)
            assert(jit.regs[i] == (skip && i == 0 ? result + 12 : skip && i == 1 ? replay + 40 : before[i]));
    };
    for (const auto* key : {"<Registry:SkipLegalScreen>", "<Registry:GoStraightToMainMenu>"}) { string(key); dispatch(true); }
    for (const auto* key : {"", "<Registry:PendingLoginChange>", "<Registry:HasBeenThroughTitleScreen>",
                           "<Registry:SkipLegalScreen>extra", "<Registry:skiplegalscreen>"}) { string(key); dispatch(false); }
    string("<Registry:SkipLegalScreen>");
    host.MemoryWrite32(input + 8, 1); dispatch(false);
    host.MemoryWrite32(input + 8, 4097); dispatch(false);
    string("<Registry:SkipLegalScreen>"); host.memory[data + std::strlen("<Registry:SkipLegalScreen>")] = 1; dispatch(false);
    string("<Registry:SkipLegalScreen>"); host.MemoryWrite32(input, UINT32_MAX - 8); dispatch(false);
    dispatch(false, 0); dispatch(false, UINT32_MAX - 8); dispatch(false, input + 1);
    string("<Registry:SkipLegalScreen>"); dispatch(false, input, 0); dispatch(false, input, UINT32_MAX - 8);
    dispatch(false, input, input + 257);
    std::puts("PASS: exact startup flags, untouched login/other keys and registers, bounded FString, ABI guards and allocation failures");
}

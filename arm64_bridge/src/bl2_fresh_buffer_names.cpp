#include "bl2_fresh_buffer_names.h"

#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#if defined(BL2_NATIVE_ARM32)
#include "../../arm32_compat/src/native_vulkan_renderer.h"
#endif

#include <GLES3/gl3.h>

#include <array>
#include <cstdint>
#include <iterator>

#if !defined(BL2_NATIVE_ARM32)
bool Bl2GlWorkerTakeBufferName(uint32_t* name);  // GL worker, bound thread only
#endif

namespace {
constexpr uint32_t kImageSize = 0x03089000;
constexpr uint32_t kGenRva = 0x01E3EEE8;
constexpr uint32_t kPoolRva = 0x03058C20;
constexpr char kGenSymbol[] =
    "_ZN15TOpenGLNamePoolI20TOpenGLBufferNameGenLj512ELj32768EE3GenEv";

struct Constructor {
    const char* symbol;
    const char* thunk;
    uint32_t entry;
    uint32_t site;
    uint32_t cache_load;
};
constexpr Constructor kConstructors[]{
    {"_ZN13TOpenGLBufferIL17ERHIResourceTypes11ELj34962EXadL_Z21CachedBindArrayBufferjjEEEC2EmjPKvjj",
     "bl2_fresh_persistent_vertex_name", 0x01E3F048, 0x01E3F0C8, 0xE5973010},
    {"_ZN13TOpenGLBufferIL17ERHIResourceTypes10ELj34963EXadL_Z28CachedBindElementArrayBufferjjEEEC2EmjPKvjj",
     "bl2_fresh_persistent_index_name", 0x01E3F254, 0x01E3F2D4, 0xE5973014},
    {"_ZN13TOpenGLBufferIL17ERHIResourceTypes0ELj35052EXadL_Z27CachedBindPixelUnpackBufferjjEEEC2EmjPKvjj",
     "bl2_fresh_persistent_pixel_name", 0x01E3F460, 0x01E3F4E0, 0xE597300C},
};

// Includes both bitfield inserts, flags/store initialization, the exact
// displaced call, and its original return path. The following cache load is
// the only difference between the three constructor windows.
constexpr uint32_t kWindow[]{
    0xE7C0E012, 0xE584C018, 0xE59F21A4, 0xE20EE0F9,
    0xE5841020, 0xE7C3E196, 0xE08F7007, 0xE584C024,
    0xE2800008, 0xE584C028, 0xE584C02C, 0xE5840000,
    0xE5970120, 0xE584C030, 0xE584301C, 0xE5C4E034,
    0xE7953002, 0xE12FFF33, 0, 0xE1A06000,
    0xE5840008, 0xE1500003, 0x0A000005, 0xE59F3154,
};

bool Span(const DynarmicHost& host, uint32_t address, size_t bytes) {
    return address >= 0x10000 &&
           uint64_t{address} + bytes <= host.GetGuestMemorySize();
}

ThunkHandler AllocationHandler(uint32_t site, uint32_t gen, uint32_t pool) {
    return [site, gen, pool](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
        auto& registers = jit.Regs();
        const uint32_t resource = registers[4];
        // A32 LDR adds these registers modulo 2^32. The displaced BLX target
        // is pinned too; do not redirect an unexpected guest function.
        const uint32_t slot = registers[5] + registers[2];
        if ((jit.Cpsr() & 0x20u) || !Span(host, resource, 0x38) ||
            !Span(host, slot, 4) || host.MemoryRead32(slot) != gen ||
            registers[0] != host.MemoryRead32(pool)) {
            HORN_LOGE("Arm32Bridge_BufferNames",
                      "Persistent name hook rejected ABI at %08X resource=%08X slot=%08X",
                      site, resource, slot);
            jit.HaltExecution(Dynarmic::HaltReason::MemoryAbort);
            return;
        }
        const bool persistent = (host.MemoryRead8(resource + 0x34) & 8u) != 0;
        GLuint name = 0;
        if (persistent) {
            // Never consume/reallocate an existing pooled mutable object.
            // No binding, GL error consumption, storage or mapping changes.
#if defined(BL2_NATIVE_ARM32)
            if (bl2vk::IsInitialized()) bl2vk::GenBuffers(1, &name);
            else
#else
            // The GL worker's bound thread takes a name generated in advance.
            if (!Bl2GlWorkerTakeBufferName(&name))
#endif
            glGenBuffers(1, &name);
            if (!name) {
                HORN_LOGE("Arm32Bridge_BufferNames",
                          "Native glGenBuffers returned zero at %08X", site);
                jit.HaltExecution(Dynarmic::HaltReason::MemoryAbort);
                return;
            }
        }
        // Reproduce the displaced LDR/BLX state exactly. Ordinary resources
        // execute the original pool Gen once, including its own side effects.
        // Persistent resources return the native name at that same callsite.
        registers[3] = gen;
        registers[14] = site + 8;
        registers[15] = persistent ? site + 8 : gen;
        if (persistent) registers[0] = name;
    };
}
} // namespace

bool InstallBl2FreshPersistentBufferNames(DynarmicHost& host) {
    auto& loader = host.GetElfLoader();
    auto& thunks = host.GetThunkManager();
    const uint32_t base = loader.GetBaseAddress();
    if (loader.GetImageSize() != kImageSize || !Span(host, base, kImageSize) ||
        loader.GetSymbolAddress(kGenSymbol) != base + kGenRva ||
        loader.GetSymbolAddress("GBufferNamePool") != base + kPoolRva) {
        HORN_LOGE("Arm32Bridge_BufferNames", "Persistent name hook rejected guest image/symbols");
        return false;
    }
    std::array<uint32_t, std::size(kConstructors)> veneers{};
    bool installed = false;
    for (size_t index = 0; index < std::size(kConstructors); ++index) {
        const auto& constructor = kConstructors[index];
        const uint32_t site = base + constructor.site;
        const uint32_t registered = thunks.FindRegisteredSymbol(constructor.thunk);
        const bool active = registered && host.MemoryRead32(site) == 0xE51FF004 &&
                            host.MemoryRead32(site + 4) == registered;
        if (index == 0) installed = active;
        if (active != installed ||
            loader.GetSymbolAddress(constructor.symbol) != base + constructor.entry ||
            host.MemoryRead32(base + constructor.entry) != 0xE92D43F8 ||
            host.MemoryRead32(base + constructor.entry + 4) != 0xE1A04000) return false;
        for (size_t word = 0; word < std::size(kWindow); ++word) {
            uint32_t expected = word == 18 ? constructor.cache_load : kWindow[word];
            if (active && word == 16) expected = 0xE51FF004;
            if (active && word == 17) expected = registered;
            if (host.MemoryRead32(site - 0x40 + uint32_t(word * 4)) != expected) {
                HORN_LOGE("Arm32Bridge_BufferNames",
                          "Persistent name hook rejected constructor %u word %u",
                          unsigned(index), unsigned(word));
                return false;
            }
        }
    }
    if (installed) return true;
    // Validate every constructor before making any guest code changes.
    for (size_t index = 0; index < std::size(kConstructors); ++index) {
        const auto& constructor = kConstructors[index];
        veneers[index] = thunks.RegisterThunk(constructor.thunk,
            AllocationHandler(base + constructor.site, base + kGenRva, base + kPoolRva));
        if (!veneers[index]) return false;
    }
    for (size_t index = 0; index < std::size(kConstructors); ++index) {
        const uint32_t site = base + kConstructors[index].site;
        host.MemoryWrite32(site, 0xE51FF004);
        host.MemoryWrite32(site + 4, veneers[index]);
        host.InvalidateGuestCode(site, 8);
        if (host.MemoryRead32(site) != 0xE51FF004 ||
            host.MemoryRead32(site + 4) != veneers[index]) return false;
    }
    HORN_LOGI("Arm32Bridge_BufferNames",
              "Persistent VBO/EBO/PBO names use fresh native GenBuffers; ordinary pool unchanged");
    return true;
}

#if defined(HORN_GL_SELF_CHECK)
#include <cstdio>
#include <optional>

namespace {
struct AllocationSnapshot {
    std::array<uint32_t, 16> registers;
    std::array<uint32_t, 64> vfp;
    uint32_t cpsr, fpscr;
    bool operator==(const AllocationSnapshot&) const = default;
};
AllocationSnapshot Snapshot(const Dynarmic::A32::Jit& jit) {
    return {jit.Regs(), jit.ExtRegs(), jit.Cpsr(), jit.Fpscr()};
}

struct AllocationFixture final : Dynarmic::A32::UserCallbacks {
    DynarmicHost& host;
    ThunkHandler handler;
    Dynarmic::A32::Jit* jit = nullptr;
    unsigned fault = 0, hook_calls = 0, exceptions = 0;
    AllocationSnapshot rejected_before{}, rejected_after{};
    explicit AllocationFixture(DynarmicHost& memory, ThunkHandler callback)
        : host(memory), handler(std::move(callback)) {}
    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        if (!Span(host, address, 4)) return std::nullopt;
        return host.MemoryRead32(address);
    }
    uint8_t MemoryRead8(uint32_t p) override { return host.MemoryRead8(p); }
    uint16_t MemoryRead16(uint32_t p) override { return host.MemoryRead16(p); }
    uint32_t MemoryRead32(uint32_t p) override { return host.MemoryRead32(p); }
    uint64_t MemoryRead64(uint32_t p) override { return host.MemoryRead64(p); }
    void MemoryWrite8(uint32_t p, uint8_t v) override { host.MemoryWrite8(p, v); }
    void MemoryWrite16(uint32_t p, uint16_t v) override { host.MemoryWrite16(p, v); }
    void MemoryWrite32(uint32_t p, uint32_t v) override { host.MemoryWrite32(p, v); }
    void MemoryWrite64(uint32_t p, uint64_t v) override { host.MemoryWrite64(p, v); }
    void InterpreterFallback(uint32_t, size_t) override {
        ++exceptions; jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }
    void ExceptionRaised(uint32_t, Dynarmic::A32::Exception) override {
        ++exceptions; jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }
    void CallSVC(uint32_t id) override {
        if (id == 1) {
            ++hook_calls;
            if (fault == 1) jit->Regs()[4] = 0;
            if (fault == 2) jit->Regs()[5] = -jit->Regs()[2];
            if (fault == 3) jit->Regs()[0] += 4;
            if (fault == 4) jit->SetCpsr(jit->Cpsr() | 0x20u);
            if (fault == 5) host.MemoryWrite32(jit->Regs()[5] + jit->Regs()[2], 0x10000);
            rejected_before = Snapshot(*jit);
            handler(*jit, host);
            rejected_after = Snapshot(*jit);
        } else {
            if (id != 2) ++exceptions;
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
    }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1000000; }
};
} // namespace

// Runs the same production callback through real A32 SVC dispatch. The
// baseline executes the original LDR/BLX and a tiny side-effecting guest Gen.
// A separate EGL runner supplies a real current native context.
extern "C" __attribute__((visibility("default"))) bool Bl2FreshPersistentBufferNamesSelfCheck() {
    DynarmicHost host;
    if (!host.Initialize()) return false;
    const uint32_t code = host.GuestAlloc(256, 16);
    const uint32_t data = host.GuestAlloc(256, 16);
    if (!code || !data) return false;
    const uint32_t site = code, veneer = code + 0x40, gen = code + 0x80;
    const uint32_t resource = data, pool = data + 0x40, global = data + 0x44,
                   slot = data + 0x48, stack = data + 0xF0;
    const uint32_t original_gen[]{0xE590C000, 0xE28CC001, 0xE580C000,
                                 0xE59F0000, 0xE12FFF1E, 0x13579BDF};
    for (size_t i = 0; i < std::size(original_gen); ++i)
        host.MemoryWrite32(gen + uint32_t(i * 4), original_gen[i]);
    host.MemoryWrite32(site, 0xE7953002);
    host.MemoryWrite32(site + 4, 0xE12FFF33);
    host.MemoryWrite32(site + 8, 0xEF000002);
    host.MemoryWrite32(veneer, 0xEF000001);
    host.MemoryWrite32(veneer + 4, 0xE12FFF1E);
    host.MemoryWrite32(global, pool);

    AllocationFixture callbacks(host, AllocationHandler(site, gen, global));
    Dynarmic::ExclusiveMonitor monitor(1);
    Dynarmic::A32::UserConfig config;
    config.callbacks = &callbacks;
    config.global_monitor = &monitor;
    config.processor_id = 0;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.always_little_endian = true;
    config.enable_cycle_counting = false;
    config.code_cache_size = 2 * 1024 * 1024;
    Dynarmic::A32::Jit jit(config);
    callbacks.jit = &jit;
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool passed, const char* label) {
        ++checks;
        failures += !passed;
        std::printf("FRESH_NAME_CHECK %s %s\n", passed ? "PASS" : "FAIL", label);
    };
    const auto prepare = [&](uint8_t flags, unsigned fault) {
        jit.Reset();
        for (size_t i = 0; i < jit.Regs().size(); ++i) jit.Regs()[i] = 0xA1100000u + uint32_t(i);
        for (size_t i = 0; i < jit.ExtRegs().size(); ++i) jit.ExtRegs()[i] = 0x71230000u + uint32_t(i);
        jit.SetCpsr(0xA80A0010);
        jit.SetFpscr(0x01000000);
        jit.Regs()[0] = pool; jit.Regs()[2] = 0xFFFF32C4;
        jit.Regs()[4] = resource; jit.Regs()[5] = slot - jit.Regs()[2];
        jit.Regs()[13] = stack; jit.Regs()[15] = site;
        host.MemoryWrite32(slot, gen); host.MemoryWrite32(pool, 0);
        host.MemoryWrite8(resource + 0x34, flags);
        callbacks.fault = fault; callbacks.hook_calls = callbacks.exceptions = 0;
    };
    prepare(0, 0);
    check(jit.Run() == Dynarmic::HaltReason::UserDefined1, "original guest Gen returns");
    const auto baseline = Snapshot(jit);
    check(host.MemoryRead32(pool) == 1 && callbacks.hook_calls == 0, "original Gen side effect exactly once");
    host.MemoryWrite32(site, 0xE51FF004);
    host.MemoryWrite32(site + 4, veneer);
    jit.InvalidateCacheRange(site, 8);
    for (uint8_t flags : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{4}, uint8_t{0xF0}}) {
        prepare(flags, 0);
        check(jit.Run() == Dynarmic::HaltReason::UserDefined1 && !callbacks.exceptions,
              "ordinary hook reaches original return site");
        check(Snapshot(jit) == baseline && host.MemoryRead32(pool) == 1 && callbacks.hook_calls == 1,
              "ordinary hook matches original full A32/VFP state and single pool Gen");
    }
    for (uint8_t flags : {uint8_t{8}, uint8_t{0x0B}, uint8_t{0xFC}}) {
        prepare(flags, 0);
        auto expected = Snapshot(jit);
        // The callback must not consume an already pending native error.
        glEnable(0xFFFFFFFFu);
        check(jit.Run() == Dynarmic::HaltReason::UserDefined1 && !callbacks.exceptions,
              "persistent hook reaches original return site");
        check(glGetError() == GL_INVALID_ENUM, "native Gen preserves prior GL error");
        const GLuint name = jit.Regs()[0];
        check(name != 0 && host.MemoryRead32(pool) == 0 && callbacks.hook_calls == 1,
              "persistent hook returns real fresh name without consuming ordinary pool");
        expected.registers[0] = name; expected.registers[3] = gen;
        expected.registers[14] = site + 8; expected.registers[15] = site + 12;
        check(Snapshot(jit) == expected, "persistent hook preserves arguments/SP/CPSR/VFP and exact BLX return");
        glBindBuffer(GL_ARRAY_BUFFER, name);
        check(glIsBuffer(name) == GL_TRUE && glGetError() == GL_NO_ERROR,
              "production fresh name creates an actual native buffer");
        glDeleteBuffers(1, &name);
    }
    for (unsigned fault = 1; fault <= 5; ++fault) {
        prepare(0x0B, fault);
        check(jit.Run() == Dynarmic::HaltReason::MemoryAbort && !callbacks.exceptions,
              "invalid resource/GOT/pool/mode/target halts before guest continuation");
        check(callbacks.rejected_after == callbacks.rejected_before &&
              host.MemoryRead32(pool) == 0 && callbacks.hook_calls == 1,
              "rejected ABI leaves complete CPU state and ordinary pool unchanged");
    }
    std::printf("FRESH_NAME_ABI status=%s checks=%u failures=%u\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures == 0;
}
#endif

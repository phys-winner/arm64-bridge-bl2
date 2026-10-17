// Production-linked regression; independent native FP and integer literal models.
#include "fpcr_check_fixture.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <memory>
#include <vector>
#include <unistd.h>

// Prevent the cached Android JitMap diagnostic from opening an app-path file
// if the game profiling property remains enabled. Test-only linker wrapper.
extern "C" int __wrap___system_property_get(const char*, char* value) { value[0] = '\0'; return 0; }


namespace {
using U32 = std::uint32_t;
constexpr U32 kData = 0xE000, kStop = 0x800, kFpcrMask = 0x07FF9F00;
constexpr std::size_t kMemorySize = 1 << 16;
unsigned cases = 0, fpcr_cases = 0, literal_cases = 0;
const char* phase = "initialization";
[[noreturn]] void Fail(const char* message) {
    std::fprintf(stderr, "FPCR_LITERAL_SELFCHECK FAIL phase=%s case=%u %s\n", phase, cases + 1, message);
    std::exit(1);
}
void Timeout(int) {
    constexpr char message[] = "FPCR_LITERAL_SELFCHECK FAIL bounded alarm expired\n";
    const auto ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
    _exit(124);
}
std::uint64_t NativeFpcr() { std::uint64_t result; asm volatile("mrs %0, fpcr" : "=r"(result)); return result; }
struct Snapshot {
    std::array<U32, 16> regs{};
    std::array<U32, 64> ext{};
    U32 cpsr = 0, fpscr = 0;
    std::array<U32, 4> data{};
    unsigned reads = 0, writes = 0, svc = 0;
};
struct Program { U32 entry; bool thumb = false; };
struct Range { U32 start, end; };
class Machine final : public Dynarmic::A32::UserCallbacks {
public:
    explicit Machine(bool fastmem, bool endian_test = false) : memory(kMemorySize) {
        for (std::size_t i = 0; i < std::size(kFpcrCode); ++i) Put(0x1000 + U32(i * 4), kFpcrCode[i]);
        Dynarmic::A32::UserConfig config;
        config.callbacks = this;
        config.arch_version = Dynarmic::A32::ArchVersion::v7;
        config.define_unpredictable_behaviour = true;
        config.always_little_endian = !endian_test;
        config.enable_cycle_counting = false;
        config.code_cache_size = 16 << 20;
        if (fastmem) config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(memory.data());
        jit = std::make_unique<Dynarmic::A32::Jit>(config);
    }
    template<class T> T Read(U32 a) const {
        if (std::uint64_t(a) + sizeof(T) > memory.size()) Fail("read outside closed fixture");
        T result; std::memcpy(&result, memory.data() + a, sizeof(result)); return result;
    }
    template<class T> void Put(U32 a, T value) {
        if (std::uint64_t(a) + sizeof(T) > memory.size()) Fail("write outside closed fixture");
        std::memcpy(memory.data() + a, &value, sizeof(value));
    }
    void CheckCallbackFpcr() {
        if (!running) return;
        const auto actual = U32(NativeFpcr()) & kFpcrMask;
        const auto expected = jit->Fpscr() & kFpcrMask;
        if (actual != expected) {
            std::fprintf(stderr, "callback_fpcr=%08X expected=%08X guest_fpscr=%08X pc=%08X\n", actual, expected, jit->Fpscr(), jit->Regs()[15]);
            Fail("guest FPCR not restored before native callback");
        }
        ++callback_fpcr_checks;
    }
    std::optional<U32> MemoryReadCode(U32 a) override { return Read<U32>(a); }
    bool IsReadOnlyMemory(U32 a) override {
        for (const auto& range : readonly) if (a >= range.start && std::uint64_t(a) + 8 <= range.end) { folded_address = a; return true; }
        return false;
    }
    void CountRead(U32 a) {
        // ConstantMemoryReads asks IsReadOnlyMemory immediately before its
        // compile-time callback. Jit may compile before entering guest FPCR.
        if (a == folded_address) { folded_address = ~U32{0}; ++folded_reads; return; }
        CheckCallbackFpcr(); reads += running;
        if (!running) Fail("unexpected data read outside execution/folding");
    }
    std::uint8_t MemoryRead8(U32 a) override { CountRead(a); return Read<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(U32 a) override { CountRead(a); return Read<std::uint16_t>(a); }
    U32 MemoryRead32(U32 a) override { CountRead(a); return Read<U32>(a); }
    std::uint64_t MemoryRead64(U32 a) override { CountRead(a); return Read<std::uint64_t>(a); }
    void MemoryWrite8(U32 a, std::uint8_t v) override { CheckCallbackFpcr(); ++writes; Put(a, v); }
    void MemoryWrite16(U32 a, std::uint16_t v) override { CheckCallbackFpcr(); ++writes; Put(a, v); }
    void MemoryWrite32(U32 a, U32 v) override { CheckCallbackFpcr(); ++writes; Put(a, v); }
    void MemoryWrite64(U32 a, std::uint64_t v) override { CheckCallbackFpcr(); ++writes; Put(a, v); }
    void InterpreterFallback(U32 pc, std::size_t) override { std::fprintf(stderr, "fallback_pc=%08X\n", pc); Fail("unexpected interpreter fallback"); }
    void ExceptionRaised(U32 pc, Dynarmic::A32::Exception e) override { std::fprintf(stderr, "exception_pc=%08X kind=%u\n", pc, unsigned(e)); Fail("unexpected guest exception"); }
    void CallSVC(U32 value) override {
        CheckCallbackFpcr();
        if (value != 0x5A) Fail("wrong SVC exit");
        svc = value; jit->Regs()[15] = kStop; jit->HaltExecution();
    }
    void AddTicks(std::uint64_t) override { Fail("cycle-counting callback in production configuration"); }
    std::uint64_t GetTicksRemaining() override { Fail("cycle-counting callback in production configuration"); }
    Snapshot Run(Program p, const Snapshot& initial) {
        jit->Reset(); jit->Regs() = initial.regs; jit->ExtRegs() = initial.ext;
        jit->Regs()[15] = p.entry;
        jit->SetCpsr((initial.cpsr & ~0x20u) | (p.thumb ? 0x20u : 0));
        jit->SetFpscr(initial.fpscr);
        for (unsigned i = 0; i < initial.data.size(); ++i) Put(kData + 4 * i, initial.data[i]);
        reads = writes = svc = 0;
        const auto host_fpcr = NativeFpcr();
        running = true; jit->Run(); running = false;
        if (svc != 0x5A || jit->Regs()[15] != kStop) Fail("bounded fixture did not exit");
        if (NativeFpcr() != host_fpcr) Fail("host FPCR not restored on Jit exit");
        Snapshot result{jit->Regs(), jit->ExtRegs(), jit->Cpsr(), jit->Fpscr(), {}, reads, writes, svc};
        for (unsigned i = 0; i < result.data.size(); ++i) result.data[i] = Read<U32>(kData + 4 * i);
        return result;
    }
    std::vector<std::uint8_t> memory;
    std::vector<Range> readonly;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    unsigned callback_fpcr_checks = 0, folded_reads = 0;
private:
    bool running = false;
    unsigned reads = 0, writes = 0, svc = 0;
    U32 folded_address = ~U32{0};
};
void Compare(const Snapshot& a, const Snapshot& b, bool callbacks = true) {
    if (a.regs != b.regs || a.ext != b.ext || a.cpsr != b.cpsr || a.fpscr != b.fpscr || a.data != b.data || a.svc != b.svc ||
        (callbacks && (a.reads != b.reads || a.writes != b.writes))) {
        for (unsigned i = 0; i < 16; ++i) if (a.regs[i] != b.regs[i]) std::fprintf(stderr, "r%u actual=%08X reference=%08X\n", i, a.regs[i], b.regs[i]);
        for (unsigned i = 0; i < 64; ++i) if (a.ext[i] != b.ext[i]) std::fprintf(stderr, "s%u actual=%08X reference=%08X\n", i, a.ext[i], b.ext[i]);
        std::fprintf(stderr, "cpsr=%08X/%08X fpscr=%08X/%08X reads=%u/%u writes=%u/%u svc=%u/%u\n", a.cpsr, b.cpsr, a.fpscr, b.fpscr, a.reads, b.reads, a.writes, b.writes, a.svc, b.svc);
        Fail("full architectural state/memory differs");
    }
    ++cases;
}
constexpr std::array<U32, 13> kBits{0u, 0x80000000u, 1u, 0x807FFFFFu, 0x00800000u, 0x3F800000u, 0xBF800001u,
    0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC12345u, 0x7F812345u};
Snapshot Initial(unsigned pattern, U32 mode, U32 sticky) {
    Snapshot s;
    for (unsigned i = 0; i < s.regs.size(); ++i) s.regs[i] = 0x11335577u ^ (i * 0x1020304u);
    for (unsigned i = 0; i < s.ext.size(); ++i) s.ext[i] = kBits[(pattern + i) % kBits.size()];
    // Write sticky/NZCV flags while keeping the control mode, then compare
    // its reset/accumulation. All16 modes are selected at native Run entry.
    s.regs[0] = kData; s.regs[1] = mode | (sticky ^ 0xA800009Fu);
    s.regs[2] = 3; s.regs[13] = kData + 0x100; s.regs[14] = k_fpcr_exit; s.regs[15] = kStop;
    s.cpsr = 0x080A0010u | ((pattern & 15) << 28); s.fpscr = mode | sticky;
    for (unsigned i = 0; i < s.data.size(); ++i) s.data[i] = kBits[(pattern + i + 3) % kBits.size()];
    s.svc = 0x5A;
    return s;
}
U32 Mode(unsigned index) { return ((index & 3) << 22) | (((index >> 2) & 1) << 24) | (((index >> 3) & 1) << 25); }

// These helpers execute AArch64 hardware math independently of Dynarmic.
// No C++ floating-point expressions or value conversions are used. VMLA is
// deliberately two rounded operations; VFMA uses the fused FMLA instruction.
enum class Math { Add, Sub, Mul, Fma };
void SetNativeFpcr(std::uint64_t value) { asm volatile("msr fpcr, %0" : : "r"(value) : "memory"); }
std::uint64_t NativeFpsr() { std::uint64_t value; asm volatile("mrs %0, fpsr" : "=r"(value)); return value; }
void SetNativeFpsr(std::uint64_t value) { asm volatile("msr fpsr, %0" : : "r"(value) : "memory"); }
void VectorMath(Math math, U32* dst, const U32* a, const U32* b) {
#define VECTOR_MATH(operation) asm volatile( \
    "ldr q0, [%0]\nldr q1, [%1]\nldr q2, [%2]\n" operation "\nstr q0, [%0]" \
    : : "r"(dst), "r"(a), "r"(b) : "v0", "v1", "v2", "memory")
    switch (math) {
    case Math::Add: VECTOR_MATH("fadd v0.4s, v1.4s, v2.4s"); break;
    case Math::Sub: VECTOR_MATH("fsub v0.4s, v1.4s, v2.4s"); break;
    case Math::Mul: VECTOR_MATH("fmul v0.4s, v1.4s, v2.4s"); break;
    case Math::Fma: VECTOR_MATH("fmla v0.4s, v1.4s, v2.4s"); break;
    }
#undef VECTOR_MATH
}
U32 ScalarMath(bool divide, U32 a, U32 b) {
    U32 result;
    if (divide) asm volatile("fmov s0, %w1\nfmov s1, %w2\nfdiv s0, s0, s1\nfmov %w0, s0"
                            : "=r"(result) : "r"(a), "r"(b) : "v0", "v1");
    else asm volatile("fmov s0, %w1\nfmov s1, %w2\nfadd s0, s0, s1\nfmov %w0, s0"
                      : "=r"(result) : "r"(a), "r"(b) : "v0", "v1");
    return result;
}
Snapshot NativeReference(U32 entry, const Snapshot& initial, bool fastmem) {
    constexpr U32 fpsr_mask = 0x0800009F, mode_mask = 0x07F70000;
    const auto saved_fpcr = NativeFpcr(), saved_fpsr = NativeFpsr();
    Snapshot s = initial;
    SetNativeFpsr(s.fpscr & fpsr_mask);
    U32 mode = s.fpscr & mode_mask, nzcv = s.fpscr & 0xF0000000;
    const auto fpscr = [&] { return mode | nzcv | U32(NativeFpsr() & fpsr_mask); };
    const auto vector = [&](Math math, unsigned dst, unsigned a, unsigned b) {
        // A32 Advanced SIMD always uses round-to-nearest, FZ and DN.
        // The fixture contains f32 and no AHP/FZ16 mode bits.
        SetNativeFpcr(0x03000000);
        VectorMath(math, s.ext.data() + 4 * dst, s.ext.data() + 4 * a, s.ext.data() + 4 * b);
    };
    const auto scalar = [&] {
        SetNativeFpcr(mode);
        s.ext[24] = ScalarMath(false, s.ext[25], s.ext[26]);
        s.ext[28] = ScalarMath(true, s.ext[29], s.ext[30]);
    };
    const auto prefix = [&](bool subtract) {
        vector(Math::Add, 0, 1, 2); vector(Math::Mul, 0, 0, 2);
        if (subtract) vector(Math::Sub, 0, 0, 1);
        vector(Math::Fma, 0, 1, 2);
    };
    if (entry == k_fpcr_math) {
        prefix(true);
        std::array<U32, 4> product{};
        SetNativeFpcr(0x03000000);
        VectorMath(Math::Mul, product.data(), s.ext.data() + 4, s.ext.data() + 8);
        VectorMath(Math::Add, s.ext.data(), s.ext.data(), product.data());
        vector(Math::Add, 3, 0, 1); vector(Math::Mul, 3, 3, 2);
    } else if (entry == k_fpcr_mixed) {
        prefix(true); scalar();
        s.ext[27] = s.data[0]; s.data[1] = s.ext[24];
        vector(Math::Fma, 0, 1, 2); vector(Math::Add, 0, 0, 2);
        s.regs[4] = fpscr();
        vector(Math::Add, 3, 0, 1); vector(Math::Mul, 3, 3, 2);
        s.data[2] = s.ext[0]; s.data[3] = s.ext[1];
        // A32 VSTR d0 is two architectural 32-bit memory accesses.
        s.reads = !fastmem; s.writes = fastmem ? 0 : 3;
    } else if (entry == k_fpcr_changed) {
        prefix(false);
        mode = s.regs[1] & mode_mask; nzcv = s.regs[1] & 0xF0000000;
        SetNativeFpsr(s.regs[1] & fpsr_mask); scalar();
        vector(Math::Add, 3, 0, 1); vector(Math::Mul, 3, 3, 2); vector(Math::Fma, 3, 1, 2);
        s.regs[4] = fpscr(); s.ext[27] = s.data[0]; s.data[1] = s.ext[24];
        s.reads = !fastmem; s.writes = !fastmem;
    } else if (entry == k_fpcr_terminal) {
        prefix(true);
    } else if (entry == k_fpcr_pressure) {
        vector(Math::Add, 0, 1, 2);
        for (unsigned q = 3; q < 16; ++q)
            vector((q % 3 == 0) ? Math::Sub : (q % 3 == 1) ? Math::Mul : Math::Add, q, 1, 2);
        vector(Math::Fma, 0, 1, 2);
        for (unsigned q = 3; q < 16; ++q) vector(Math::Fma, q, 1, 2);
        SetNativeFpcr(mode); s.ext[24] = ScalarMath(false, s.ext[25], s.ext[26]);
        s.data[2] = s.ext[0]; s.data[3] = s.ext[1]; s.writes = fastmem ? 0 : 2;
    } else Fail("unknown reference kernel");
    s.fpscr = fpscr();
    SetNativeFpcr(saved_fpcr); SetNativeFpsr(saved_fpsr);
    return s;
}
void CheckFpcr() {
    constexpr std::array<U32, 4> sticky{0, 1, 0x9F, 0xA800009F};
    constexpr std::array<U32, 5> entries{k_fpcr_math, k_fpcr_mixed, k_fpcr_changed, k_fpcr_terminal, k_fpcr_pressure};
    for (bool fastmem : {false, true}) {
        Machine candidate{fastmem};
        phase = fastmem ? "fpcr-production-fastmem" : "fpcr-callback-boundaries";
        for (U32 entry : entries) for (unsigned mode = 0; mode < 16; ++mode) for (U32 flags : sticky) for (unsigned bits = 0; bits < kBits.size(); ++bits) {
            const auto initial = Initial(bits, Mode(mode), flags);
            const auto reference = NativeReference(entry, initial, fastmem);
            const auto actual = candidate.Run({entry}, initial);
            Compare(actual, reference); ++fpcr_cases;
        }
        std::printf("FPCR_LITERAL phase=%s cases=%u callback_fpcr_checks=%u native_reference=1\n",
                    phase, fpcr_cases, candidate.callback_fpcr_checks);
    }
}
struct Literal { Program program; U32 address; unsigned dst; bool wide, immutable, conditional; };
U32 EncodeVldr(bool up, bool wide, unsigned dst, bool conditional, unsigned imm8, unsigned rn = 15) {
    const unsigned D = wide ? dst >> 4 : dst & 1, Vd = wide ? dst & 15 : dst >> 1;
    return (conditional ? 0u : 0xE0000000u) | 0x0D100A00u | (U32(up) << 23) | (D << 22) | (rn << 16) | (Vd << 12) | (U32(wide) << 8) | imm8;
}
std::vector<Literal> MakeLiterals(Machine& machine) {
    std::vector<Literal> fixtures;
    U32 cursor = 0x3000;
    for (unsigned kind = 0; kind < 3; ++kind) for (bool up : {false, true}) for (bool wide : {false, true}) for (bool immutable : {false, true}) for (unsigned variant = 0; variant < 2; ++variant) {
        const bool thumb = kind != 0, it = kind == 2;
        const U32 entry = cursor + (thumb && variant ? 2u : 0u), loadpc = entry + (it ? 2u : 0u);
        const U32 pcbase = thumb ? (loadpc + 4u) & ~3u : loadpc + 8u;
        const unsigned imm8 = 8, dst = !thumb && variant ? 31u : 0;
        const U32 address = up ? pcbase + 4 * imm8 : pcbase - 4 * imm8;
        if (address < loadpc + 8 && address + 12 > entry) Fail("literal span overlaps fixture code");
        const bool conditional = !thumb || it;
        const U32 word = EncodeVldr(up, wide, dst, !thumb, imm8);
        if (thumb) {
            if (it) machine.Put(entry, std::uint16_t(0xBF08));
            machine.Put(loadpc, std::uint16_t(word >> 16)); machine.Put(loadpc + 2, std::uint16_t(word));
            machine.Put(loadpc + 4, std::uint16_t(0xDF5A)); machine.Put(loadpc + 6, std::uint16_t(0xBF00));
        } else { machine.Put(loadpc, word); machine.Put(loadpc + 4, 0xEF00005Au); }
        machine.Put(address, U32(0x7F812345u)); machine.Put(address + 4, U32(0x80000001u));
        machine.Put(address + 8, U32(0));
        if (immutable) machine.readonly.push_back({address, address + 12});
        fixtures.push_back({{entry, thumb}, address, dst, wide, immutable, conditional});
        cursor += 0x80;
    }
    return fixtures;
}
void CheckLiterals() {
    phase = "vldr-production-literal";
    Machine candidate{false, true};
    const auto fixtures = MakeLiterals(candidate);
    for (const auto& fixture : fixtures) for (unsigned mode = 0; mode < 16; ++mode) for (bool big : {false, true}) for (bool predicate : {false, true}) {
        auto initial = Initial((mode + big + predicate) % kBits.size(), Mode(mode), 0xA800009Fu);
        initial.cpsr = (initial.cpsr & ~0x40000220u) | (U32(predicate) << 30) | (U32(big) << 9) | (fixture.program.thumb ? 0x20u : 0u);
        if (!fixture.immutable) {
            candidate.Put(fixture.address, kBits[(mode + predicate) % kBits.size()]);
            candidate.Put(fixture.address + 4, kBits[(mode + predicate + 5) % kBits.size()]);
        }
        const auto actual = candidate.Run(fixture.program, initial);
        auto model = initial;
        const bool executed = !fixture.conditional || predicate;
        if (executed) {
            U32 lo = candidate.Read<U32>(fixture.address), hi = candidate.Read<U32>(fixture.address + 4);
            if (big) { lo = __builtin_bswap32(lo); hi = __builtin_bswap32(hi); }
            if (fixture.wide && big) std::swap(lo, hi);
            model.ext[fixture.wide ? 2 * fixture.dst : fixture.dst] = lo;
            if (fixture.wide) model.ext[2 * fixture.dst + 1] = hi;
        }
        Compare(actual, model, false); ++literal_cases;
        const unsigned expected_reads = executed && !fixture.immutable ? (fixture.wide ? 2u : 1u) : 0u;
        if (actual.reads != expected_reads || actual.writes) Fail("literal folding or predicate changed callbacks");
        ++cases; ++literal_cases;
    }
    if (!candidate.folded_reads) Fail("readonly literal folding was not exercised");
    std::printf("VLDR_LITERAL production_linked=1 integer_model=1 checks=%u folded_reads=%u\n", literal_cases, candidate.folded_reads);
}
}
int main(int argc, char**) {
    if (argc != 1) Fail("unexpected argument");
    std::signal(SIGALRM, Timeout); alarm(30);
    CheckFpcr(); CheckLiterals();
    alarm(0);
    std::printf("FPCR_LITERAL_SELFCHECK overall=PASS fpcr_cases=%u literal_checks=%u total_checks=%u full_bits_compared=1 native_reference=1 production_linked=1 app_io=0\n", fpcr_cases, literal_cases, cases);
}

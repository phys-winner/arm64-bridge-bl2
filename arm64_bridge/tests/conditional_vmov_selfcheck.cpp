// Standalone bit-exact Dynarmic VMOV check; no game/app/device I/O.
#include "culling_original_words.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <unistd.h>

namespace {
using U32 = std::uint32_t;
constexpr U32 kStop = 0x800;
constexpr std::size_t kMemorySize = 1 << 20;
constexpr U32 kObject = 0xC0000, kView = 0xC1000, kRenderer = 0xC3000;
constexpr U32 kGlobal0 = 0xC4000, kGlobal1 = 0xC4004, kStack = 0xC5000;
constexpr unsigned kNoException = ~0u;
unsigned cases = 0;
const char* phase = "initialization";
static_assert(kStack + 16 <= kMemorySize);

void Timeout(int) {
    constexpr char message[] = "VMOV_SELECT_SELFCHECK FAIL bounded alarm expired\n";
    const auto ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
    _exit(124);
}

[[noreturn]] void Fail(const char* message, unsigned index = 0) {
    std::fprintf(stderr, "VMOV_SELECT_SELFCHECK FAIL phase=%s case=%u %s\n",
                 phase, index ? index : cases + 1, message);
    std::exit(1);
}

struct Snapshot {
    std::array<U32, 16> regs{};
    std::array<U32, 64> ext{};
    U32 cpsr = 0, fpscr = 0, svc = 0;
    bool exception = false;
    unsigned exception_kind = kNoException;
};
struct Program { U32 entry; bool thumb; };
struct Move { bool wide; unsigned cond, dst, src; };

bool Condition(unsigned cond, U32 cpsr) {
    const bool n = (cpsr >> 31) & 1, z = (cpsr >> 30) & 1;
    const bool c = (cpsr >> 29) & 1, v = (cpsr >> 28) & 1;
    switch (cond) {
    case 0: return z; case 1: return !z; case 2: return c; case 3: return !c;
    case 4: return n; case 5: return !n; case 6: return v; case 7: return !v;
    case 8: return c && !z; case 9: return !c || z; case 10: return n == v;
    case 11: return n != v; case 12: return !z && n == v; case 13: return z || n != v;
    case 14: return true; default: return false;
    }
}

U32 EncodeMove(Move m) {
    if (m.wide) return (m.cond << 28) | 0x0EB00B40u |
        ((m.dst >> 4) << 22) | ((m.dst & 15) << 12) | ((m.src >> 4) << 5) | (m.src & 15);
    return (m.cond << 28) | 0x0EB00A40u |
        ((m.dst & 1) << 22) | ((m.dst >> 1) << 12) | ((m.src & 1) << 5) | (m.src >> 1);
}

U32 EncodeVselGe(unsigned dst, unsigned yes, unsigned no) {
    return 0xFE200A00u | ((dst & 1) << 22) | ((dst >> 1) << 12) |
           ((yes & 1) << 7) | ((yes >> 1) << 16) | ((no & 1) << 5) | (no >> 1);
}

bool ModelMove(Snapshot& s, Move m) {
    if (m.cond == 15) return false;
    if (!Condition(m.cond, s.cpsr)) return true;
    const unsigned length = ((s.fpscr >> 16) & 7) + 1;
    const unsigned encoded_stride = (s.fpscr >> 20) & 3;
    if (encoded_stride == 1 || encoded_stride == 2) return false;
    const unsigned stride = encoded_stride == 3 ? 2 : 1;
    const unsigned bank = m.wide ? 4 : 8;
    if (stride * length > bank || (length == 1 && stride != 1)) return false;
    const auto scalar = [m](unsigned r) {
        return m.wide ? r < 4 || (r >= 16 && r < 20) : r < 8;
    };
    const unsigned count = scalar(m.dst) ? 1 : length;
    const bool source_scalar = scalar(m.src);
    auto dst = m.dst, src = m.src;
    const auto next = [bank, stride](unsigned r) { return r / bank * bank + (r % bank + stride) % bank; };
    for (unsigned i = 0; i < count; ++i) {
        const unsigned d = m.wide ? dst * 2 : dst;
        const unsigned a = m.wide ? src * 2 : src;
        const U32 low = s.ext[a], high = m.wide ? s.ext[a + 1] : 0;
        s.ext[d] = low;
        if (m.wide) s.ext[d + 1] = high;
        dst = next(dst);
        if (!source_scalar) src = next(src);
    }
    return true;
}

class Machine final : public Dynarmic::A32::UserCallbacks {
public:
    explicit Machine(bool production) : memory(kMemorySize) {
        Dynarmic::A32::UserConfig config;
        config.callbacks = this;
        config.arch_version = Dynarmic::A32::ArchVersion::v7;
        config.always_little_endian = true;
        config.define_unpredictable_behaviour = production;
        config.enable_cycle_counting = !production;
        if (production) {
            // The fixture buffer never resizes after this construction. Every
            // test code/data access stays in its checked 1-MiB guest span.
            config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(memory.data());
        }
        jit = std::make_unique<Dynarmic::A32::Jit>(config);
    }
    template<class T> T Read(U32 a) const {
        if (static_cast<std::uint64_t>(a) + sizeof(T) > memory.size()) Fail("out of range read");
        T value; std::memcpy(&value, memory.data() + a, sizeof(value)); return value;
    }
    template<class T> void Put(U32 a, T value) {
        if (static_cast<std::uint64_t>(a) + sizeof(T) > memory.size()) Fail("out of range write");
        std::memcpy(memory.data() + a, &value, sizeof(value));
    }
    void ValidateCullingFixture(const Snapshot& s) const {
        const auto contains = [this](U32 a, std::size_t length) {
            return static_cast<std::uint64_t>(a) + length <= memory.size();
        };
        // These are the complete data bases read by the immutable 83-word
        // culling body. No computed FP result is used as a memory address.
        if (!contains(s.regs[4], 0x68) || !contains(s.regs[7], 0x5C) ||
            !contains(s.regs[13], 12) || !contains(s.regs[10], 4) || !contains(s.regs[11], 4))
            Fail("culling fixture base outside fastmem span");
        const auto view = static_cast<std::uint64_t>(Read<U32>(s.regs[7] + 0x58)) +
                          Read<U32>(s.regs[13] + 8);
        if (view + 0x414 > memory.size()) Fail("culling fixture view outside fastmem span");
    }
    std::uint8_t MemoryRead8(U32 a) override { return Read<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(U32 a) override { return Read<std::uint16_t>(a); }
    U32 MemoryRead32(U32 a) override { return Read<U32>(a); }
    std::uint64_t MemoryRead64(U32 a) override { return Read<std::uint64_t>(a); }
    void MemoryWrite8(U32, std::uint8_t) override { Fail("unexpected guest store"); }
    void MemoryWrite16(U32, std::uint16_t) override { Fail("unexpected guest store"); }
    void MemoryWrite32(U32, U32) override { Fail("unexpected guest store"); }
    void MemoryWrite64(U32, std::uint64_t) override { Fail("unexpected guest store"); }
    void InterpreterFallback(U32, std::size_t) override { Fail("unexpected interpreter fallback"); }
    void ExceptionRaised(U32 pc, Dynarmic::A32::Exception e) override {
        const auto kind = static_cast<unsigned>(e);
        if (kind != expected_exception_kind || pc != active_program.entry) {
            std::fprintf(stderr,
                "phase=%s case=%u exception_kind=%u expected_kind=%u pc=%08X entry=%08X opcode=%08X cpsr=%08X fpscr=%08X thumb=%u\n",
                phase, cases + 1, kind, expected_exception_kind, pc, active_program.entry,
                Read<U32>(pc & ~3u), jit->Cpsr(), jit->Fpscr(), active_program.thumb);
            Fail("unexpected exception kind/location", cases + 1);
        }
        exception_kind = kind;
        exception = true; jit->HaltExecution();
    }
    void CallSVC(U32 svc) override {
        if (svc != 0x5A && svc != 0x5B) Fail("wrong exit SVC");
        exit_svc = svc; completed = true; jit->Regs()[15] = kStop; jit->HaltExecution();
    }
    void AddTicks(std::uint64_t ticks) override { remaining = ticks < remaining ? remaining - ticks : 0; }
    std::uint64_t GetTicksRemaining() override { return remaining; }
    Program Add(const std::vector<U32>& words) {
        const U32 entry = cursor;
        for (U32 w : words) { Put(cursor, w); cursor += 4; }
        cursor = (cursor + 15) & ~15u;
        if (cursor >= kObject) Fail("test code exhausted");
        return {entry, false};
    }
    Program AddThumb(const std::vector<std::uint16_t>& halfwords) {
        const U32 entry = cursor;
        for (auto w : halfwords) { Put(cursor, w); cursor += 2; }
        cursor = (cursor + 15) & ~15u;
        if (cursor >= kObject) Fail("test code exhausted");
        return {entry, true};
    }
    Snapshot Run(Program p, const Snapshot& initial, std::uint64_t budget = 10000,
                 bool step = false, unsigned wanted_exception_kind = kNoException) {
        jit->Reset();
        jit->Regs() = initial.regs; jit->ExtRegs() = initial.ext;
        jit->Regs()[15] = p.entry;
        jit->SetCpsr((initial.cpsr & ~0x20u) | (p.thumb ? 0x20u : 0));
        jit->SetFpscr(initial.fpscr);
        remaining = budget; completed = false; exception = false; exit_svc = 0;
        active_program = p; expected_exception_kind = wanted_exception_kind;
        exception_kind = kNoException;
        if (step) {
            for (unsigned i = 0; i < 16 && !completed && !exception; ++i) {
                jit->Step();
                jit->ClearHalt(Dynarmic::HaltReason::Step);
            }
        } else {
            jit->Run();
        }
        if (!completed && !exception) Fail("run did not halt");
        Snapshot result{jit->Regs(), jit->ExtRegs(), jit->Cpsr(), jit->Fpscr(), exit_svc, exception, exception_kind};
        // Distinct reference programs have distinct instruction addresses; callbacks normalize their terminal PC.
        if (completed && result.regs[15] != kStop) Fail("wrong terminal PC");
        result.regs[15] = kStop;
        return result;
    }
    std::vector<std::uint8_t> memory;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
private:
    U32 cursor = 0x1000, exit_svc = 0;
    std::uint64_t remaining = 0;
    bool completed = false, exception = false;
    Program active_program{};
    unsigned expected_exception_kind = kNoException, exception_kind = kNoException;
};

U32 rng = 0x41109980;
U32 Random() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
Snapshot Initial(unsigned flags, U32 fpscr = 0xA800009Fu) {
    Snapshot s;
    for (auto& r : s.regs) r = Random();
    for (auto& r : s.ext) r = Random();
    s.regs[13] = kStack; s.regs[15] = kStop;
    s.cpsr = (flags << 28) | 0x080A0010u; s.fpscr = fpscr; s.svc = 0x5A;
    return s;
}

void Compare(const Snapshot& actual, const Snapshot& expected) {
    ++cases;
    if (actual.exception != expected.exception) Fail("exception/fallback differs", cases);
    if (actual.exception_kind != expected.exception_kind) Fail("exception kind differs", cases);
    if (actual.regs != expected.regs) {
        for (unsigned i = 0; i < 16; ++i) if (actual.regs[i] != expected.regs[i])
            std::fprintf(stderr, "r%u actual=%08X expected=%08X\n", i, actual.regs[i], expected.regs[i]);
        Fail("GPR differs", cases);
    }
    if (actual.ext != expected.ext) {
        for (unsigned i = 0; i < 64; ++i) if (actual.ext[i] != expected.ext[i])
            std::fprintf(stderr, "s%u actual=%08X expected=%08X\n", i, actual.ext[i], expected.ext[i]);
        Fail("FP bit pattern differs", cases);
    }
    if (actual.cpsr != expected.cpsr) {
        std::fprintf(stderr, "cpsr actual=%08X expected=%08X\n", actual.cpsr, expected.cpsr);
        Fail("CPSR differs", cases);
    }
    if (actual.fpscr != expected.fpscr) {
        std::fprintf(stderr, "fpscr actual=%08X expected=%08X\n", actual.fpscr, expected.fpscr);
        Fail("FPSCR differs", cases);
    }
    if (actual.svc != expected.svc) Fail("continuation differs", cases);
}

void BitModelCases(Machine& machine) {
    phase = "scalar_all_conditions";
    for (bool wide : {false, true}) for (unsigned cond = 0; cond <= 14; ++cond) {
        const std::array<std::array<unsigned, 2>, 4> pairs = wide
            ? std::array<std::array<unsigned, 2>, 4>{{{8, 16}, {8, 8}, {8, 9}, {31, 1}}}
            : std::array<std::array<unsigned, 2>, 4>{{{10, 16}, {10, 10}, {10, 9}, {31, 0}}};
        for (const auto& pair : pairs) {
            Move move{wide, cond, pair[0], pair[1]};
            const Program program = machine.Add({EncodeMove(move), 0xEF00005A});
            for (unsigned flags = 0; flags < 16; ++flags) {
                auto initial = Initial(flags), expected = initial;
                if (!ModelMove(expected, move)) Fail("invalid scalar model");
                Compare(machine.Run(program, initial), expected);
            }
        }
    }
    constexpr std::array<U32, 8> bits{0, 0x80000000, 1, 0x007FFFFF, 0x7F800000, 0x7FC01234, 0x7FA54321, 0xFF812345};
    phase = "special_bits_fp_modes";
    for (bool wide : {false, true}) for (unsigned mode = 0; mode < 16; ++mode) {
        Move move{wide, 10, 8, 9};
        const Program program = machine.Add({EncodeMove(move), 0xEF00005A});
        for (U32 payload : bits) for (unsigned flags : {0u, 8u}) {
            auto initial = Initial(flags, 0xF800009Fu | (mode << 22));
            initial.ext[wide ? 18 : 9] = payload;
            if (wide) initial.ext[19] = payload ^ 0x01234567u;
            auto expected = initial; ModelMove(expected, move);
            Compare(machine.Run(program, initial), expected);
        }
    }
    // Legal/illegal deprecated vector modes retain the original conditional-before-validation path.
    constexpr std::array<U32, 8> vector_modes{0x00010000,0x00030000,0x00310000,0x00040000,0x00300000,0x00070000,0x00100000,0x00200000};
    phase = "vector_len_stride_fallback";
    for (bool wide : {false, true}) for (U32 mode : vector_modes)
        for (unsigned cond : {0u,1u,10u,11u}) for (unsigned dst : {0u,8u}) {
            Move move{wide, cond, dst, 9};
            const Program program = machine.Add({EncodeMove(move), 0xEF00005A});
            for (unsigned flags = 0; flags < 16; ++flags) {
                auto initial = Initial(flags, 0xA800009Fu | mode), expected = initial;
                if (!ModelMove(expected, move)) {
                    expected.exception = true; expected.svc = 0;
                    expected.exception_kind = static_cast<unsigned>(Dynarmic::A32::Exception::UnpredictableInstruction);
                }
                Compare(machine.Run(program, initial, 10000, false, expected.exception_kind), expected);
            }
        }
    // NV goes through DecodeVFP's unconditional table, where conditional VMOV
    // does not match. Preserve the existing UndefinedInstruction path.
    phase = "nv_undefined_encoding";
    const Program nv = machine.Add({EncodeMove({false,15,10,16}), 0xEF00005A});
    auto initial = Initial(0), expected = initial; expected.exception = true; expected.svc = 0;
    expected.exception_kind = static_cast<unsigned>(Dynarmic::A32::Exception::UndefinedInstruction);
    Compare(machine.Run(nv, initial, 10000, false, expected.exception_kind), expected);
    phase = "single_step_scalar";
    for (bool wide : {false, true}) for (unsigned cond = 0; cond < 15; ++cond) {
        Move move{wide, cond, 8, 9};
        const Program program = machine.Add({EncodeMove(move), 0xEF00005A});
        for (unsigned flags = 0; flags < 16; ++flags) {
            auto state = Initial(flags), wanted = state; ModelMove(wanted, move);
            Compare(machine.Run(program, state, 10000, true), wanted);
        }
    }
}

void MixedAndThumbCases(Machine& machine) {
    phase = "prior_conditional_integer";
    for (bool wide : {false, true}) for (unsigned first : {0u,1u,10u}) for (unsigned cond = 0; cond <= 14; ++cond) {
        Move a{wide, cond, 8, 9}, b{!wide, 11, 10, 16};
        const Program program = machine.Add({(first << 28) | 0x01A04005u, EncodeMove(a), EncodeMove(b), 0xEF00005A});
        for (unsigned flags = 0; flags < 16; ++flags) {
            auto initial = Initial(flags), expected = initial;
            if (Condition(first, expected.cpsr)) expected.regs[4] = expected.regs[5];
            ModelMove(expected, a); ModelMove(expected, b);
            Compare(machine.Run(program, initial), expected);
        }
    }
    phase = "thumb_it_fallback";
    for (bool wide : {false, true}) for (unsigned cond : {0u,1u,6u,7u,10u,11u})
        for (unsigned mask : {8u,4u,12u,2u,10u,1u,7u,15u}) {
            std::vector<std::uint16_t> code{static_cast<std::uint16_t>(0xBF00 | cond << 4 | mask)};
            std::vector<Move> moves;
            unsigned it = cond << 4 | mask, i = 0;
            while (it) {
                Move m{wide, it >> 4, 8 + i % 3, 9 + i % 3};
                moves.push_back(m); m.cond = 14;
                const U32 word = EncodeMove(m);
                code.push_back(word >> 16); code.push_back(word & 0xFFFF);
                it = (it & 7) == 0 ? 0 : (it & 0xE0) | ((it << 1) & 0x1F);
                ++i;
            }
            code.push_back(0xDF5A);
            const Program program = machine.AddThumb(code);
            for (unsigned flags = 0; flags < 16; ++flags) {
                auto initial = Initial(flags), expected = initial; expected.cpsr |= 0x20;
                for (const auto& m : moves) ModelMove(expected, m);
                Compare(machine.Run(program, initial), expected);
            }
        }
}

std::vector<U32> CullingCode(bool vsel, bool loop) {
    std::vector<U32> code(kCullingOriginal.begin(), kCullingOriginal.end());
    unsigned replaced = 0;
    if (vsel) for (auto& w : code) {
        if ((w & 0x0FBF0FD0u) != 0x0EB00A40u || (w >> 28) >= 14) continue;
        const unsigned dst = ((w >> 12) & 15) * 2 + ((w >> 22) & 1);
        const unsigned src = (w & 15) * 2 + ((w >> 5) & 1);
        if ((w >> 28) == 10) w = EncodeVselGe(dst, src, dst);
        else if ((w >> 28) == 11) w = EncodeVselGe(dst, dst, src);
        else Fail("unexpected culling VMOV condition");
        ++replaced;
    }
    if (vsel && replaced != 15) Fail("wrong culling replacement count");
    if (loop) {
        code.back() = 0xE1A00000; // Final BGT replaced by NOP solely in the benchmark loop.
        code.push_back(0xE2500001); // SUBS r0,r0,#1
        const int branch_index = static_cast<int>(code.size());
        code.push_back(0x1A000000 | (static_cast<U32>(-branch_index - 2) & 0xFFFFFF));
        code.push_back(0xEF00005A);
    } else {
        code.back() = 0xCA000000; // BGT to second exit; current_pc+8 lands at the next-but-one word.
        code.push_back(0xEF00005A); code.push_back(0xEF00005B);
    }
    return code;
}

Snapshot CullingInput(Machine& machine, unsigned test) {
    auto s = Initial(test & 15, 0xA800009Fu | ((test % 16) << 22));
    s.regs[4] = kObject; s.regs[7] = kRenderer; s.regs[10] = kGlobal0;
    s.regs[11] = kGlobal1; s.ext[16] = 0; s.ext[18] = 0x3F800000; s.ext[19] = 0x40000000;
    machine.Put(kRenderer + 0x58, kView); machine.Put(kView + 4, 0x12345678u); machine.Put(kStack + 8, 0u);
    const auto number = [](U32 seed) { return static_cast<float>(static_cast<int>(seed % 20001) - 10000) / 32.0f; };
    for (unsigned lane = 0; lane < 3; ++lane) {
        const float center = number(Random()), extent = static_cast<float>(Random() % 1001) / 32.0f;
        float origin = number(Random());
        if (test % 8 == 0) origin = center;
        machine.Put(kObject + 0x3C + lane * 4, center); machine.Put(kObject + 0x48 + lane * 4, extent);
        machine.Put(kView + 0x2E0 + lane * 4, origin);
    }
    machine.Put(kGlobal0, test % 2 ? 1.0f : 0.5f); machine.Put(kGlobal1, 1.0f);
    machine.Put(kView + 0x410, 1.0f);
    machine.Put(kObject + 0x5C, test % 3 ? 1000000.0f : 0.0f);
    machine.Put(kObject + 0x64, test % 5 ? 0.0f : 100.0f);
    return s;
}

void CullingCasesAndBenchmark(Machine& machine, bool benchmark) {
    phase = "complete_culling_reference";
    const Program original = machine.Add(CullingCode(false, false));
    const Program reference = machine.Add(CullingCode(true, false));
    for (unsigned test = 0; test < 512; ++test) {
        auto initial = CullingInput(machine, test);
        machine.ValidateCullingFixture(initial);
        const auto a = machine.Run(original, initial), b = machine.Run(reference, initial);
        Compare(a, b);
    }
    if (!benchmark) return;
    phase = "culling_microbench";
    const Program loop_original = machine.Add(CullingCode(false, true));
    const Program loop_reference = machine.Add(CullingCode(true, true));
    auto initial = CullingInput(machine, 1); initial.fpscr = 0; initial.regs[0] = 100000;
    machine.ValidateCullingFixture(initial);
    const auto measure = [&](Program p) {
        const auto before = std::chrono::steady_clock::now();
        const auto result = machine.Run(p, initial, 20000000);
        const auto after = std::chrono::steady_clock::now();
        if (result.regs[0] != 0) Fail("benchmark loop incomplete");
        return std::chrono::duration<double, std::milli>(after - before).count();
    };
    measure(loop_original); measure(loop_reference); // Warm translation before measuring.
    std::array<double, 5> a{}, b{};
    for (unsigned i = 0; i < a.size(); ++i) {
        if (i & 1) { b[i] = measure(loop_reference); a[i] = measure(loop_original); }
        else { a[i] = measure(loop_original); b[i] = measure(loop_reference); }
    }
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    std::printf("CULLING_MICROBENCH iterations=100000 samples=5 original_ms=%.6f vsel_reference_ms=%.6f ratio=%.6f\n", a[2], b[2], a[2]/b[2]);
}
} // namespace

int main(int argc, char** argv) {
    bool benchmark = false, production = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--benchmark") == 0) benchmark = true;
        else if (std::strcmp(argv[i], "--production") == 0) production = true;
        else Fail("unknown argument");
    }
    if (std::signal(SIGALRM, Timeout) == SIG_ERR) Fail("could not register bounded alarm");
    alarm(30);
    Machine machine(production);
    std::printf("VMOV_SELECT_CONFIG production=%u cycle_counting=%u fastmem=%u define_unpredictable=%u fixture_bytes=%zu alarm_seconds=30\n",
                production, !production, production, production, kMemorySize);
    BitModelCases(machine); MixedAndThumbCases(machine); CullingCasesAndBenchmark(machine, benchmark);
    std::printf("VMOV_SELECT_SELFCHECK overall=PASS cases=%u culling_cases=512 scalar_f32_f64=1 vector_fallback=1 thumb_it_fallback=1 memory_stores=0\n", cases);
    alarm(0);
    return 0;
}

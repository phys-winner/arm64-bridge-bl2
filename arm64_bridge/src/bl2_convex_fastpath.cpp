#include "bl2_convex_fastpath.h"

#include "dynarmic_host.h"
#include "elf_loader.h"
#include "guest_software_write_watch.h"
#include "obb_sha256.h"
#include "thunk_manager.h"

#include <android/log.h>
#include <arm_neon.h>
#include <dynarmic/interface/A32/a32.h>
#include <sys/system_properties.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {
enum class Kind { Box, Outcode, Contained, Sphere };
struct Function {
    const char* symbol;
    const char* thunk;
    const char* sha;
    uint32_t rva, size, bit;
    Kind kind;
    uint32_t first;
};
constexpr std::array<Function, 4> kFunctions{{
    {"_ZNK13FConvexVolume12IntersectBoxERK7FVectorS2_",
     "bl2_convex_fastpath",
     "19EB7BE076E2AC8A515349C5C7D5F6C11A5EBEE1CB8E408F343D4E269048120C",
     0x01099890, 316, 1, Kind::Box, 0xE92D08F8},
    {"_ZNK13FConvexVolume25GetBoxIntersectionOutcodeERK7FVectorS2_",
     "bl2_convex_outcode_fastpath",
     "5BED9A2A2578E29D08C4EADA60BB5602794F8F5A3C6D35812B223E6CF00BD332",
     0x01099710, 384, 1, Kind::Outcode, 0xE92D08F8},
    {"_ZNK13FConvexVolume12IntersectBoxERK7FVectorS2_Rj",
     "bl2_convex_contained_fastpath",
     "199FDF61C40113786E53740BD4D816D2D7B2A40432AC0B4CA9113C54CCD6B4E4",
     0x01099B28, 404, 2, Kind::Contained, 0xE92D09F0},
    {"_ZNK13FConvexVolume15IntersectSphereERK7FVectorRKf",
     "bl2_convex_sphere_fastpath",
     "7CCA7B26004C671DACFE9146D8F864F052B0E29D505F1C67D92D68A32A2B3C27",
     0x01099CBC, 232, 4, Kind::Sphere, 0xE92D08F8},
}};
constexpr uint32_t kLdrPc = 0xE51FF004;
constexpr size_t kObjectSize = 0x300, kPlanesSize = 0x2000;
constexpr uint32_t kCases = 128, kControls = 16;
std::atomic<uint32_t> g_installed_mask{0}, g_active_mask{0};
// Startup tests run before guest workers exist. These become immutable afterward.
bool g_testing = false, g_host_fp_preserved = true, g_guest_fpcr_synced = true;
bool g_runtime_trace_enabled = false;
std::atomic<bool> g_runtime_traced{false};
uint32_t g_native_hits = 0, g_native_fallbacks = 0;

template <class T> T Read(const uint8_t* pointer) {
    T value;
    std::memcpy(&value, pointer, sizeof(value));
    return value;
}
template <class T> void Write(uint8_t* pointer, T value) {
    std::memcpy(pointer, &value, sizeof(value));
}
bool Overlaps(uint32_t a, size_t as, uint32_t b, size_t bs) {
    return uint64_t{a} < uint64_t{b} + bs && uint64_t{b} < uint64_t{a} + as;
}
bool SafeRange(DynarmicHost& host, uint32_t address, size_t size,
               bool writable = false) {
    if (!address || (address & 3) || !size ||
        uint64_t{address} + size > host.GetGuestMemorySize()) return false;
    const uint64_t end = uint64_t{address} + size;
    // Both primary/callback stacks are GuestAlloc blocks in this anonymous RW
    // arena. File-backed mappings (including potentially inaccessible ones)
    // start at 0x80000000 and are deliberately excluded from direct access.
    if (DynarmicHost::IsHeapRange(address, size)) return true;
    const auto base = host.GetElfLoader().GetBaseAddress();
    // Complete original RW PT_LOAD, certified by the pinned image/function
    // gate below. Do not treat holes or arbitrary reserved space as inputs.
    if (uint64_t{address} >= uint64_t{base} + 0x02D209D0 &&
        end <= uint64_t{base} + 0x03088FB8) return true;
    return !writable && host.GetElfLoader().IsReadOnlyRange(address, size);
}

struct Inputs {
    uint32_t planes;
    size_t groups;
};
std::optional<Inputs> Validate(DynarmicHost& host, Kind kind,
                              uint32_t object, uint32_t center,
                              uint32_t extent, uint32_t output) {
    const size_t extent_size = kind == Kind::Sphere ? 4 : 12;
    if (!SafeRange(host, object, 0x114) || !SafeRange(host, center, 12) ||
        !SafeRange(host, extent, extent_size)) return std::nullopt;
    const auto* memory = host.GetGuestMemoryBase();
    const int32_t count = Read<int32_t>(memory + object + 0x110);
    if (count > 256) return std::nullopt;
    const uint32_t external = Read<uint32_t>(memory + object + 0x100);
    const uint32_t planes = external ? external : object + 0x80;
    const size_t groups = count > 0 ? (static_cast<size_t>(count) + 3) / 4 : 0;
    if (groups && !SafeRange(host, planes, groups * 64)) return std::nullopt;
    if (kind == Kind::Contained &&
        (!SafeRange(host, output, 4, true) ||
         Overlaps(output, 4, object, 0x114) ||
         Overlaps(output, 4, center, 12) ||
         Overlaps(output, 4, extent, extent_size) ||
         (groups && Overlaps(output, 4, planes, groups * 64)))) {
        // Stock initializes output to 1 BEFORE reading ANY input. A native
        // read-first implementation must decline aliases without writing.
        return std::nullopt;
    }
    return Inputs{planes, groups};
}

struct HostFP {
    uint64_t control, status;
};
HostFP ReadHostFP() {
    HostFP result;
    asm volatile("mrs %0, fpcr\n\tmrs %1, fpsr"
                 : "=r"(result.control), "=r"(result.status) : : "memory");
    return result;
}
class StandardNeonFP {
public:
    StandardNeonFP() : saved(ReadHostFP()) {
        // A32 ASIMD uses RN + DN + FZ regardless of guest FPSCR controls.
        // AHP/FZ16 do not affect these F32 instructions; traps are disabled.
        constexpr uint64_t standard = 0x03000000;
        asm volatile("msr fpcr, %0\n\tmsr fpsr, xzr"
                     : : "r"(standard) : "memory");
    }
    ~StandardNeonFP() { if (active) Restore(); }
    uint32_t Finish() {
        uint64_t status;
        asm volatile("mrs %0, fpsr" : "=r"(status) : : "memory");
        Restore();
        if (g_testing) {
            const auto after = ReadHostFP();
            g_host_fp_preserved &= after.control == saved.control &&
                                   after.status == saved.status;
        }
        // IOC/DZC/OFC/UFC/IXC/IDC are cumulative. FPSCR control, NZCV,
        // QC and existing sticky bits belong to the guest and stay intact.
        return static_cast<uint32_t>(status) & 0x9F;
    }
private:
    void Restore() {
        asm volatile("msr fpsr, %0\n\tmsr fpcr, %1"
                     : : "r"(saved.status), "r"(saved.control) : "memory");
        active = false;
    }
    HostFP saved;
    bool active = true;
};

float32x4_t Broadcast(const uint8_t* pointer, bool absolute = false) {
    uint32_t bits = Read<uint32_t>(pointer);
    if (absolute) bits &= 0x7FFFFFFF;
    return vreinterpretq_f32_u32(vdupq_n_u32(bits));
}
float32x4_t Abs(float32x4_t value) {
    return vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(value),
                                          vdupq_n_u32(0x7FFFFFFF)));
}
// Inline assembly makes each A32 VMLA an ordered MUL then ADD, including its
// individual rounding/exception effects. Compiler contraction cannot fuse it.
float32x4_t Distance(float32x4_t x, float32x4_t y, float32x4_t z,
                    float32x4_t w, float32x4_t cx, float32x4_t cy,
                    float32x4_t cz) {
    float32x4_t result, product;
    asm volatile(
        "fmul %0.4s, %2.4s, %6.4s\n\t"
        "fmul %1.4s, %3.4s, %7.4s\n\t"
        "fadd %0.4s, %0.4s, %1.4s\n\t"
        "fmul %1.4s, %4.4s, %8.4s\n\t"
        "fadd %0.4s, %0.4s, %1.4s\n\t"
        "fsub %0.4s, %0.4s, %5.4s"
        : "=&w"(result), "=&w"(product)
        : "w"(x), "w"(y), "w"(z), "w"(w),
          "w"(cx), "w"(cy), "w"(cz) : "memory");
    return result;
}
float32x4_t Radius(float32x4_t x, float32x4_t y, float32x4_t z,
                  float32x4_t ex, float32x4_t ey, float32x4_t ez) {
    const auto ax = Abs(x), ay = Abs(y), az = Abs(z);
    float32x4_t result, product;
    asm volatile(
        "fmul %0.4s, %2.4s, %5.4s\n\t"
        "fmul %1.4s, %3.4s, %6.4s\n\t"
        "fadd %0.4s, %0.4s, %1.4s\n\t"
        "fmul %1.4s, %4.4s, %7.4s\n\t"
        "fadd %0.4s, %0.4s, %1.4s"
        : "=&w"(result), "=&w"(product)
        : "w"(ax), "w"(ay), "w"(az),
          "w"(ex), "w"(ey), "w"(ez) : "memory");
    return result;
}
uint32x4_t Greater(float32x4_t a, float32x4_t b) {
    uint32x4_t result;
    asm volatile("fcmgt %0.4s, %1.4s, %2.4s"
                 : "=w"(result) : "w"(a), "w"(b) : "memory");
    return result;
}
uint32_t Packed(uint32x4_t value) {
    // Exact VUZP.16/VUZP.8 reduction bytes, needed by stock integer NZCV.
    return (vgetq_lane_u32(value, 0) & 0x000000FF) |
           (vgetq_lane_u32(value, 1) & 0x0000FF00) |
           (vgetq_lane_u32(value, 2) & 0x00FF0000) |
           (vgetq_lane_u32(value, 3) & 0xFF000000);
}
uint32_t CompareZeroFlags(uint32_t value, bool subtract) {
    return (value & 0x80000000) | (value ? 0 : 0x40000000) |
           (subtract ? 0x20000000 : 0);
}
uint32_t ReverseSubtractFlags(uint32_t value) {
    const uint32_t result = 1 - value;
    return (result & 0x80000000) | (result ? 0 : 0x40000000) |
           (value <= 1 ? 0x20000000 : 0) |
           ((((1 ^ value) & (1 ^ result)) >> 31) << 28);
}
struct Result {
    uint32_t value, contained, nzcv, sticky;
};
std::optional<Result> Native(DynarmicHost& host, Kind kind,
                             uint32_t object, uint32_t center,
                             uint32_t extent, uint32_t output,
                             uint32_t cpsr, uint32_t fpscr) {
    // Decline BE/IT/Jazelle and unusual VFP vector/trap modes before reads or
    // mutation. The displaced original instruction has a BE-safe veneer.
    if ((cpsr & 0x0700FE20) || (fpscr & 0x00379F00)) return std::nullopt;
    // When guest FPCR already equals the A32 ASIMD standard mode, Dynarmic
    // emits no MSR before these operations. A stale physical mode must retain
    // the original path; masking IDC would lose the actual FP contract.
    if ((fpscr & 0x03C00000) == 0x03000000 &&
        (ReadHostFP().control & 0x03C00000) != 0x03000000) return std::nullopt;
    const auto input = Validate(host, kind, object, center, extent, output);
    if (!input) return std::nullopt;
    const uint8_t* memory = host.GetGuestMemoryBase();
    const auto cx = Broadcast(memory + center);
    const auto cy = Broadcast(memory + center + 4);
    const auto cz = Broadcast(memory + center + 8);
    const auto ex = Broadcast(memory + extent, kind != Kind::Sphere);
    const auto ey = kind == Kind::Sphere ? ex : Broadcast(memory + extent + 4, true);
    const auto ez = kind == Kind::Sphere ? ex : Broadcast(memory + extent + 8, true);
    uint32x4_t outside = vdupq_n_u32(0), intersects = vdupq_n_u32(0);
    StandardNeonFP fp;
    for (size_t group = 0; group < input->groups; ++group) {
        const auto* planes = reinterpret_cast<const float*>(
            memory + input->planes + group * 64);
        const auto x = vld1q_f32(planes), y = vld1q_f32(planes + 4);
        const auto z = vld1q_f32(planes + 8), w = vld1q_f32(planes + 12);
        const auto distance = Distance(x, y, z, w, cx, cy, cz);
        const auto radius = kind == Kind::Sphere ? ex : Radius(x, y, z, ex, ey, ez);
        outside = vorrq_u32(outside, Greater(distance, radius));
        if (kind == Kind::Outcode || kind == Kind::Contained) {
            const auto negative = vreinterpretq_f32_u32(veorq_u32(
                vreinterpretq_u32_f32(radius), vdupq_n_u32(0x80000000)));
            intersects = vorrq_u32(intersects, Greater(distance, negative));
        }
        // No early-out and no tail mask: every padded lane contributes to
        // the original result AND cumulative exception flags.
    }
    const uint32_t sticky = fp.Finish();
    const uint32_t positive = Packed(outside), negative = Packed(intersects);
    if (kind == Kind::Outcode) {
        return Result{positive ? 2u : (1u | (negative ? 2u : 0u)), 0,
            positive ? CompareZeroFlags(positive, true) :
                       CompareZeroFlags(negative, false), sticky};
    }
    if (kind == Kind::Contained) {
        return Result{positive ? 0u : 1u, (positive || negative) ? 0u : 1u,
            CompareZeroFlags(positive ? positive : negative, true), sticky};
    }
    return Result{positive ? 0u : 1u, 0, ReverseSubtractFlags(positive), sticky};
}

uint32_t NextRandom(uint32_t& value) {
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    return value;
}
uint32_t Finite(uint32_t& state) {
    const auto value = NextRandom(state);
    return (value & 0x807FFFFF) | ((120 + (value % 15)) << 23);
}
struct Scratch {
    uint32_t object, planes, center, extent, output, wrapper;
};
uint32_t MakeCase(DynarmicHost& host, const Scratch& s,
                  Kind kind, uint32_t test) {
    uint8_t* memory = host.GetGuestMemoryBase();
    std::memset(memory + s.object, 0, kObjectSize);
    std::memset(memory + s.planes, 0, kPlanesSize);
    uint32_t random = 0xC0FFEE21 ^ (test * 0x9E3779B9);
    constexpr std::array<int32_t, 10> counts{-3, 0, 1, 3, 4, 5, 8, 17, 256, 257};
    int32_t count = test < counts.size() ? counts[test] :
        static_cast<int32_t>(1 + NextRandom(random) % 31);
    bool inline_planes = test % 4 == 0 && count <= 8;
    const uint32_t planes = inline_planes ? s.object + 0x80 : s.planes;
    Write<uint32_t>(memory + s.object + 0x100, inline_planes ? 0 : s.planes);
    Write<int32_t>(memory + s.object + 0x110, count);
    for (size_t lane = 0; lane < 3; ++lane) {
        Write<uint32_t>(memory + s.center + lane * 4, Finite(random));
        Write<uint32_t>(memory + s.extent + lane * 4, Finite(random));
    }
    const size_t groups = count > 0 ? (static_cast<size_t>(count) + 3) / 4 : 0;
    for (size_t word = 0; word < groups * 16; ++word)
        Write<uint32_t>(memory + planes + word * 4, Finite(random));
    constexpr std::array<uint32_t, 16> specials{
        0, 0x80000000, 0x3F800000, 0xBF800000, 0x00800000, 0x7F7FFFFF,
        1, 0x80000001, 0x7F800000, 0xFF800000, 0x7FC12345, 0xFFC12345,
        0x7F812345, 0xFF812345, 0x3F800001, 0x00800001};
    if (test >= 10 && test < 26) {
        for (size_t lane = 0; lane < 3; ++lane) {
            Write<uint32_t>(memory + s.center + lane * 4, specials[test - 10]);
            Write<uint32_t>(memory + s.extent + lane * 4, specials[(test - 9) % 16]);
        }
        for (size_t word = 0; word < groups * 16; ++word)
            Write<uint32_t>(memory + planes + word * 4, specials[(test + word) % 16]);
    }
    if (test >= 26 && test <= 41) {
        std::memset(memory + planes, 0, groups * 64);
        for (size_t lane = 0; lane < 3; ++lane) {
            Write<uint32_t>(memory + s.center + lane * 4, lane ? 0 : 0x40000000);
            Write<uint32_t>(memory + s.extent + lane * 4, lane ? 0 : 0xBF800000);
        }
        for (size_t group = 0; group < groups; ++group)
            for (size_t lane = 0; lane < 4; ++lane) {
                Write<uint32_t>(memory + planes + group * 64 + lane * 4, 0x3F800000);
                const uint32_t boundary = test % 3 == 0 ? 0x3F7FFFFF :
                    (test % 3 == 1 ? 0x3F800000 : 0x3F800001);
                Write<uint32_t>(memory + planes + group * 64 + 48 + lane * 4, boundary);
            }
        if (test == 34) {
            // Separate rounding differs from an FMA at this cancellation.
            Write<uint32_t>(memory + s.center, 0x3F7FFFFF);
            Write<uint32_t>(memory + s.center + 4, 0x3F800000);
            std::memset(memory + s.extent, 0, 12);
            Write<uint32_t>(memory + planes, 0x3F800001);
            Write<uint32_t>(memory + planes + 16, 0xBF800000);
            Write<uint32_t>(memory + planes + 48, 0);
        }
        if (test >= 38) {
            count = test == 38 ? 1 : 4;
            Write<int32_t>(memory + s.object + 0x110, count);
            std::memset(memory + planes, 0, 64);
            std::memset(memory + s.center, 0, 12);
            for (size_t lane = 0; lane < 4; ++lane)
                Write<uint32_t>(memory + planes + 48 + lane * 4,
                    test == 40 ? 0 : (test == 41 ? 0xBF800000 : 0x3F800000));
            if (test == 38) Write<uint32_t>(memory + planes + 60, 0xBF800000);
            if (test == 40) for (size_t lane = 0; lane < 4; ++lane)
                Write<uint32_t>(memory + planes + lane * 4, 0x3F800000);
        }
    }
    uint32_t output = s.output;
    if (kind == Kind::Contained && test >= 124) {
        output = test == 124 ? s.center : (test == 125 ? s.extent :
            (test == 126 ? planes + 48 : s.object + 0x110));
    }
    Write<uint32_t>(memory + s.output, 0xDEADBEEF);
    NotifyGuestMemoryWritten(s.object, kObjectSize);
    NotifyGuestMemoryWritten(s.planes, kPlanesSize);
    NotifyGuestMemoryWritten(s.center, 12);
    NotifyGuestMemoryWritten(s.extent, 12);
    NotifyGuestMemoryWritten(s.output, 4);
    return output;
}

struct Observation {
    uint32_t value = 0, output = 0, fpscr = 0, cpsr = 0;
    bool callee_preserved = false;
    bool operator==(const Observation&) const = default;
};
struct Capture {
    uint32_t requested_fpscr = 0, output = 0, saved_fpscr = 0, saved_cpsr = 0;
    uint32_t thunk = 0;
    bool preparing = false, entry_fpcr_synced = true;
    bool entering = true;
    bool active = true;
    Observation observation;
};
uint32_t MakeCaptureThunk(DynarmicHost& host, const std::shared_ptr<Capture>& c) {
    return host.GetThunkManager().RegisterThunk("bl2_convex_selftest",
        [c](Dynarmic::A32::Jit& jit, DynarmicHost& owner) {
            if (!c->active) return;
            if (c->preparing) {
                c->saved_fpscr = jit.Fpscr(); c->saved_cpsr = jit.Cpsr();
                jit.SetFpscr(c->requested_fpscr);
                c->preparing = false;
                return;
            }
            if (c->entering) {
                // Prepare in a completed RunFunction, so this Run's prelude
                // has loaded the requested mode into the physical FPCR.
                c->entry_fpcr_synced = (ReadHostFP().control & 0x07C00000) ==
                                       (c->requested_fpscr & 0x07C00000);
                g_guest_fpcr_synced &= c->entry_fpcr_synced;
                jit.SetCpsr(0x68070010); // NZCV/Q/GE sentinels, little-endian ARM
                for (size_t i = 4; i <= 11; ++i) jit.Regs()[i] = 0xA5000000 + i;
                for (size_t i = 16; i < 32; ++i) jit.ExtRegs()[i] = 0xBAD00000 + i;
                c->entering = false;
            } else {
                auto& o = c->observation;
                o.value = jit.Regs()[0]; o.output = owner.MemoryRead32(c->output);
                o.fpscr = jit.Fpscr(); o.cpsr = jit.Cpsr();
                o.callee_preserved = c->entry_fpcr_synced;
                for (size_t i = 4; i <= 11; ++i)
                    o.callee_preserved &= jit.Regs()[i] == 0xA5000000 + i;
                for (size_t i = 16; i < 32; ++i)
                    o.callee_preserved &= jit.ExtRegs()[i] == 0xBAD00000 + i;
                jit.SetFpscr(c->saved_fpscr); jit.SetCpsr(c->saved_cpsr);
                c->entering = true;
            }
        });
}
void MakeWrapper(DynarmicHost& host, uint32_t wrapper,
                 uint32_t capture, uint32_t function) {
    // Save the host caller's callee registers, capture/set FP controls, call
    // the real ARM guest, capture/restore FP state, then return normally.
    const std::array<uint32_t, 12> code{
        0xE92D5FF0, 0xED2D8B10, 0xE59FC018, 0xE12FFF3C,
        0xE59FC014, 0xE12FFF3C, 0xE59FC008, 0xE12FFF3C,
        0xECBD8B10, 0xE8BD9FF0, capture, function};
    for (size_t i = 0; i < code.size(); ++i)
        host.MemoryWrite32(wrapper + static_cast<uint32_t>(i * 4), code[i]);
    host.InvalidateGuestCode(wrapper, code.size() * 4);
}
Observation Observe(DynarmicHost& host, const Scratch& s,
                    const std::shared_ptr<Capture>& c, Kind kind,
                    uint32_t test, uint32_t control) {
    c->output = MakeCase(host, s, kind, test);
    c->requested_fpscr = ((control & 3) << 22) | ((control & 4) << 22) |
        ((control & 8) << 22) | ((test & 1) << 26) |
        (test % 5 == 0 ? 0xA800009F : 0x60000000);
    c->observation = {};
    c->entering = true;
    c->preparing = true;
    // SetFpscr changes JIT state, not the physical FPCR of an ongoing Run.
    // Starting the measured wrapper in a new Run exercises the requested
    // control mode rather than the preceding case's host mode.
    host.RunFunction(c->thunk);
    if (c->preparing) return c->observation;
    host.RunFunction(s.wrapper, {s.object, s.center, s.extent, c->output});
    if (!c->entering) c->observation.callee_preserved = false;
    return c->observation;
}
bool GuardCheck(DynarmicHost& host, const Scratch& s) {
    MakeCase(host, s, Kind::Contained, 4);
    const uint32_t before = host.MemoryRead32(s.output);
    for (auto bad : {0u, s.center + 1, 0x80000000u, 0xFFFFFFFCu}) {
        if (Validate(host, Kind::Contained, s.object, bad, s.extent, s.output))
            return false;
        if (Validate(host, Kind::Contained, s.object, s.center, s.extent, bad))
            return false;
    }
    if (Validate(host, Kind::Contained, s.object, s.center, s.extent, s.center))
        return false;
    host.MemoryWrite32(s.object + 0x100, 0x80000000);
    if (Validate(host, Kind::Box, s.object, s.center, s.extent, 0)) return false;
    return host.MemoryRead32(s.output) == before;
}
struct Hook {
    const Function* info;
    uint32_t function = 0, fallback = 0, thunk = 0, svc = 0;
    bool patched = false;
    std::vector<Observation> originals;
};
bool Certified(DynarmicHost& host, const Function& f, uint32_t address) {
    auto& elf = host.GetElfLoader();
    if (!address || uint64_t{elf.GetBaseAddress()} + f.rva != address ||
        elf.GetImageSize() != 0x03089000 ||
        uint64_t{address} + f.size > host.GetGuestMemorySize() ||
        !elf.IsReadOnlyRange(address, f.size)) return false;
    ObbSha256 sha;
    sha.Update(host.GetGuestMemoryBase() + address, f.size);
    const auto digest = ObbSha256::Hex(sha.Finalize());
    return std::strcmp(digest.data(), f.sha) == 0;
}
bool Register(DynarmicHost& host, Hook& h) {
    h.fallback = host.GuestAlloc(24, 4);
    if (!h.fallback) return false;
    for (uint32_t offset : {0u, 12u}) {
        host.MemoryWrite32(h.fallback + offset, h.info->first);
        host.MemoryWrite32(h.fallback + offset + 4, kLdrPc);
        host.MemoryWrite32(h.fallback + offset + 8,
            offset ? __builtin_bswap32(h.function + 4) : h.function + 4);
    }
    host.InvalidateGuestCode(h.fallback, 24);
    host.AddExecutableRange(h.fallback, 24);
    const auto kind = h.info->kind;
    const auto bit = h.info->bit;
    const auto rva = h.info->rva;
    const auto fallback = h.fallback;
    h.thunk = host.GetThunkManager().RegisterThunk(h.info->thunk,
        [kind, bit, rva, fallback](Dynarmic::A32::Jit& jit, DynarmicHost& owner) {
            const uint32_t cpsr = jit.Cpsr(), fpscr = jit.Fpscr();
            const auto result = (g_active_mask.load(std::memory_order_relaxed) & bit)
                ? Native(owner, kind, jit.Regs()[0], jit.Regs()[1], jit.Regs()[2],
                         jit.Regs()[3], cpsr, fpscr) : std::nullopt;
            if (!g_testing && g_runtime_trace_enabled &&
                !g_runtime_traced.exchange(true, std::memory_order_relaxed))
                __android_log_print(ANDROID_LOG_INFO, "BL2PerfCheck",
                    "convex first-runtime rva=%08X fpscr=%08X fpcr=%08X "
                    "native=%u fallback=%u", rva, fpscr,
                    static_cast<uint32_t>(ReadHostFP().control),
                    static_cast<unsigned>(result.has_value()),
                    static_cast<unsigned>(!result.has_value()));
            if (!result) {
                if (g_testing) ++g_native_fallbacks;
                jit.Regs()[15] = fallback + ((cpsr & 0x200) ? 12 : 0);
                return;
            }
            if (g_testing) ++g_native_hits;
            if (kind == Kind::Contained) owner.MemoryWrite32(jit.Regs()[3], result->contained);
            jit.Regs()[0] = result->value;
            jit.SetFpscr(fpscr | result->sticky);
            const uint32_t lr = jit.Regs()[14];
            jit.SetCpsr((cpsr & ~0xF0000020) | result->nzcv | ((lr & 1) << 5));
            jit.Regs()[15] = lr & ~1u;
        });
    if (!h.thunk) {
        host.GuestFree(h.fallback); h.fallback = 0;
        return false;
    }
    // Registered closures retain their veneers on every later failure.
    // A one-instruction SVC also lets rejected BE contexts reach the fallback;
    // an LDR-PC entry literal would byte-swap before reaching the handler.
    h.svc = host.MemoryRead32(h.thunk);
    return (h.svc & 0xFF000000) == 0xEF000000 && (h.svc & 0x00FFFFFF);
}
bool PerfTestsEnabled() {
    char value[PROP_VALUE_MAX]{};
    return __system_property_get("debug.bl2.perf_tests", value) == 1 && value[0] == '1';
}
} // namespace

void SetBl2ConvexMask(uint32_t mask) {
    g_active_mask.store(mask & g_installed_mask.load(std::memory_order_acquire) & 7,
                        std::memory_order_release);
}

bool InstallBl2ConvexFastpath(DynarmicHost& host, uint32_t startupMask) {
    char property[PROP_VALUE_MAX]{};
    uint32_t mask = startupMask & 7;
    const int property_size = __system_property_get("debug.bl2.convex_fastpath", property);
    if (property_size == 1 && property[0] >= '0' && property[0] <= '7')
        mask = static_cast<uint32_t>(property[0] - '0');
    else if (property_size > 0) return false;
    if (!mask) return true;
    uint32_t original_cases = 0, native_cases = 0, patched_cases = 0;
    uint32_t failed_rva = 0, failed_test = 0, failed_control = 0;
    Observation failed_expected{}, failed_actual{};
    std::vector<Hook> hooks;
    Scratch s{};
    std::shared_ptr<Capture> capture;
    g_testing = true; g_host_fp_preserved = true; g_guest_fpcr_synced = true;
    g_runtime_trace_enabled = false;
    g_native_hits = 0; g_native_fallbacks = 0;
    const auto finish = [&](bool passed) {
        if (!passed) {
            g_active_mask.store(0, std::memory_order_release);
            for (auto& h : hooks) if (h.patched) {
                host.MemoryWrite32(h.function, h.info->first);
                host.InvalidateGuestCode(h.function, 4);
            }
        }
        if (capture) { capture->active = false; capture->output = 0; }
        if (s.wrapper) host.InvalidateGuestCode(s.wrapper, 48);
        for (uint32_t address : {s.wrapper, s.output, s.extent, s.center, s.planes, s.object})
            if (address) host.GuestFree(address);
        g_testing = false;
        g_runtime_trace_enabled = passed && PerfTestsEnabled();
        if (PerfTestsEnabled()) __android_log_print(ANDROID_LOG_INFO, "BL2PerfCheck",
            "convex mask=%u original=%u native=%u patched=%u hits=%u fallback=%u "
            "host_fp=%u entry_fpcr=%u result=%s fail_rva=%08X case=%u control=%u "
            "expected=%08X/%08X/%08X actual=%08X/%08X/%08X",
            mask, original_cases, native_cases, patched_cases,
            g_native_hits, g_native_fallbacks, static_cast<unsigned>(g_host_fp_preserved),
            static_cast<unsigned>(g_guest_fpcr_synced),
            passed ? "PASS" : "FAIL", failed_rva, failed_test, failed_control,
            failed_expected.value, failed_expected.fpscr, failed_expected.cpsr,
            failed_actual.value, failed_actual.fpscr, failed_actual.cpsr);
        return passed;
    };
    if (host.IsJitRunning() || g_installed_mask.load()) return finish(false);
    for (const auto& f : kFunctions) if (mask & f.bit) {
        Hook hook{}; hook.info = &f; hook.function = host.FindSymbol(f.symbol);
        if (!Certified(host, f, hook.function)) return finish(false);
        hooks.push_back(std::move(hook));
    }
    s = {host.GuestAlloc(kObjectSize, 16), host.GuestAlloc(kPlanesSize, 16),
         host.GuestAlloc(16, 16), host.GuestAlloc(16, 16),
         host.GuestAlloc(16, 16), host.GuestAlloc(48, 16)};
    if (!s.object || !s.planes || !s.center || !s.extent || !s.output || !s.wrapper ||
        !GuardCheck(host, s)) return finish(false);
    host.AddExecutableRange(s.wrapper, 48);
    capture = std::make_shared<Capture>();
    const uint32_t capture_thunk = MakeCaptureThunk(host, capture);
    if (!capture_thunk) return finish(false);
    capture->thunk = capture_thunk;
    g_active_mask.store(mask, std::memory_order_release);
    for (auto& h : hooks) {
        h.originals.reserve(kCases * kControls);
        MakeWrapper(host, s.wrapper, capture_thunk, h.function);
        for (uint32_t test = 0; test < kCases; ++test)
            for (uint32_t control = 0; control < kControls; ++control) {
                const auto original = Observe(host, s, capture, h.info->kind, test, control);
                ++original_cases;
                if (!original.callee_preserved) return finish(false);
                h.originals.push_back(original);
            }
        if (!Register(host, h)) return finish(false);
        MakeWrapper(host, s.wrapper, capture_thunk, h.thunk);
        size_t i = 0;
        for (uint32_t test = 0; test < kCases; ++test)
            for (uint32_t control = 0; control < kControls; ++control) {
                const auto native = Observe(host, s, capture, h.info->kind, test, control);
                ++native_cases;
                const auto& expected = h.originals[i++];
                if (!(native == expected) || !g_host_fp_preserved) {
                    failed_rva = h.info->rva; failed_test = test; failed_control = control;
                    failed_expected = expected; failed_actual = native;
                    return finish(false);
                }
            }
    }
    for (auto& h : hooks) {
        host.MemoryWrite32(h.function, h.svc);
        host.InvalidateGuestCode(h.function, 4);
        h.patched = true;
        MakeWrapper(host, s.wrapper, capture_thunk, h.function);
        size_t i = 0;
        for (uint32_t test = 0; test < kCases; ++test)
            for (uint32_t control = 0; control < kControls; ++control) {
                const auto patched = Observe(host, s, capture, h.info->kind, test, control);
                ++patched_cases;
                const auto& expected = h.originals[i++];
                if (!(patched == expected) || !g_host_fp_preserved) {
                    failed_rva = h.info->rva; failed_test = test; failed_control = control;
                    failed_expected = expected; failed_actual = patched;
                    return finish(false);
                }
            }
    }
    g_installed_mask.store(mask, std::memory_order_release);
    return finish(true);
}

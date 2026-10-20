#include "native_host.h"
#include "native_vfs.h"
#include "../../arm64_bridge/src/thunk_manager.h"
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if !defined(__arm__)
#error "This check must execute natively on ARM32."
#endif

extern "C" void native_check_registers(std::uint32_t entry, std::uint32_t* output);
extern "C" void native_check_clobber();
bool NativeSha256SelfCheck();
bool NativeGlMappingSelfCheck();

// The hook interrupts ARM instructions rather than an ordinary ABI call, so
// caller-saved registers, flags and live VFP values must survive its callback.
asm(R"(
.syntax unified
.arm
.fpu neon
.text
.align 2
.global native_check_registers
.type native_check_registers,%function
native_check_registers:
    push {r4-r11,lr}
    vpush {d8-d15}
    sub sp,sp,#12
    str r0,[sp]
    str r1,[sp,#4]
    vmrs r2,fpscr
    str r2,[sp,#8]
    mov r2,#0
    vmsr fpscr,r2
    mov r0,#1
    mov r1,#2
    mov r2,#3
    mov r3,#4
    mov r4,#5
    mov r5,#6
    mov r6,#7
    mov r7,#8
    mov r8,#9
    mov r9,#10
    mov r10,#11
    mov r11,#12
    mov r12,#13
    vmov d0,r0,r1
    vmov d1,r2,r3
    vmov d7,r4,r5
    vmov d15,r6,r7
    vmov d31,r8,r9
    cmp r0,r0
    adr lr,1f
    ldr pc,[sp]
1:
    ldr lr,[sp,#4]
    stmia lr,{r0-r12}
    mrs r0,apsr
    str r0,[lr,#52]
    vmrs r0,fpscr
    str r0,[lr,#56]
    add r0,lr,#60
    vstmia r0,{d0-d1}
    add r0,lr,#76
    vstmia r0,{d7}
    add r0,lr,#84
    vstmia r0,{d15}
    add r0,lr,#92
    vstmia r0,{d31}
    ldr r0,[sp,#8]
    vmsr fpscr,r0
    add sp,sp,#12
    vpop {d8-d15}
    pop {r4-r11,pc}
.size native_check_registers,.-native_check_registers
.global native_check_clobber
.type native_check_clobber,%function
native_check_clobber:
    mov r0,#42
    mov r1,#43
    mov r2,#44
    mov r3,#45
    mov r12,#46
    vmov d0,r0,r1
    vmov d1,r2,r3
    vmov d7,r0,r12
    vmov d31,r1,r12
    cmp r0,r1
    mov r0,#0x01000000
    vmsr fpscr,r0
    bx lr
.size native_check_clobber,.-native_check_clobber
)");

extern "C" __attribute__((pcs("aapcs"))) std::uint32_t native_check_args7(
    std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d,
    std::uint32_t e, std::uint32_t f, std::uint32_t g) {
    return a + 3*b + 5*c + 7*d + 11*e + 13*f + 17*g;
}
extern "C" __attribute__((pcs("aapcs"))) float native_check_float_stack(
    std::uint32_t a, float b, float c, float d, float e, float f, std::uint32_t g) {
    return static_cast<float>(a + g) + b + c + d + e + f;
}
extern "C" __attribute__((pcs("aapcs"))) std::uint64_t native_check_return64(
    std::uint32_t low, std::uint32_t high) {
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

static std::uint32_t float_word(float value) {
    std::uint32_t result = 0;
    std::memcpy(&result,&value,sizeof(result));
    return result;
}
template<class T> static std::uint32_t address(T value) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(value));
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(NativeSha256SelfCheck());
    std::puts("PASS: SHA-256 empty, abc, two-block padding and streamed million-a vectors");
    assert(NativeVfsSelfCheck());
    std::puts("PASS: native VFS 64-bit bounds, overflow and legacy path boundaries");
    auto& host = GetNativeHost();
    assert(host.Initialize());
    assert(NativeGlMappingSelfCheck());
    std::puts("PASS: real GLES 2x2 positive/zero occlusion queries, RESULT/NO_WAIT/AVAILABLE and preserved GL errors");
    std::puts("PASS: real GLES coherent mappings, partial shadows, GPU visibility, fences and shared-context deletion");
    const std::array<std::uint32_t,2> words{0xE1A00000,0xE12FFF1E};
    const auto continuation = host.CreateCodeVeneer(words);
    assert(continuation);
    unsigned callbacks = 0;
    const auto core = Dynarmic::A32::Jit::NativeCoreIdentity();
    const auto entry = host.GetThunkManager().RegisterThunk("native_host_check",
        [&](Dynarmic::A32::Jit& frame, DynarmicHost&) {
            for (std::size_t i = 0; i < 13; ++i) assert(frame.Regs()[i] == i + 1);
            assert((frame.Regs()[13] & 7) == 0);
            assert((frame.Cpsr() & 0xF0000000u) == 0x60000000u);
            assert(frame.Fpscr() == 0);
            assert(Dynarmic::A32::Jit::NativeCoreIdentity() == core);
            ++callbacks;
            native_check_clobber();
            frame.Regs()[15] = continuation;
        });
    assert(entry);
    const std::array<std::uint32_t,2> patchedWords{0xE51FF004,entry};
    const auto patchedSite = host.CreateCodeVeneer(patchedWords);
    assert(patchedSite);
    std::array<std::uint32_t,25> registers{};
    native_check_registers(patchedSite,registers.data());
    assert(callbacks == 1);
    for (std::size_t i = 0; i < 13; ++i) assert(registers[i] == i + 1);
    assert((registers[13] & 0xF0000000u) == 0x60000000u);
    assert(registers[14] == 0);
    for (std::size_t i = 15; i < registers.size(); ++i) assert(registers[i] == i - 14);
    const auto editEntry = host.GetThunkManager().RegisterThunk("native_host_check_edit",
        [&](Dynarmic::A32::Jit& frame, DynarmicHost&) {
            frame.Regs()[0] = 99;
            frame.ExtRegs()[0] = 42;
            frame.ExtRegs()[30] = 43;
            frame.ExtRegs()[62] = 44;
            frame.SetCpsr((frame.Cpsr() & ~0xF80F0000u) | 0x28050000u);
            frame.SetFpscr(0x01000000u);
            frame.Regs()[15] = continuation;
        });
    assert(editEntry);
    native_check_registers(editEntry,registers.data());
    assert(registers[0] == 99 && registers[15] == 42 && registers[21] == 43 && registers[23] == 44);
    assert((registers[13] & 0xF80F0000u) == 0x28050000u);
    assert(registers[14] == 0x01000000u);
    assert(host.RunGuestCallback(address(native_check_args7),{1,2,3,4,5,6,7}) == 302);
    assert(host.RunGuestCallback(address(native_check_float_stack),
        {1,float_word(0.5f),float_word(1.25f),float_word(-2.0f),float_word(4.0f),float_word(8.75f),6}) ==
        float_word(19.5f));
    assert(host.RunGuestCallback64(address(native_check_return64),{0x89ABCDEF,0x01234567}) ==
        0x0123456789ABCDEFULL);
    std::puts("PASS: native ARM hooks preserve and edit real APSR/FPSCR, registers and VFP; stable core identity");
    std::puts("PASS: seven-word calls, stack floats and R0:R1 returns");
}

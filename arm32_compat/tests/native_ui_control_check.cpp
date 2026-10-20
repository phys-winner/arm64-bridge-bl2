#include "native_host.h"
#include "../../arm64_bridge/src/thunk_manager.h"
#include "../../arm64_bridge/src/bl2_native_resolution.h"
#include "../../arm64_bridge/src/bl2_pause_debug_menu.h"
#include "../../arm64_bridge/src/bl2_touch_gfx_transport.h"
#include "../../arm64_bridge/src/bl2_touch_settings.h"
#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <thread>

#if !defined(__arm__) || !defined(BL2_NATIVE_ARM32)
#error "Run this control check through the real native ARM32 host."
#endif

extern "C" void native_ui_check_frame(std::uint32_t entry,std::uint32_t* output);
asm(R"(
.syntax unified
.arm
.fpu neon
.text
.align 2
.global native_ui_check_frame
.type native_ui_check_frame,%function
native_ui_check_frame:
    push {r4-r11,lr}
    vpush {d8-d15}
    sub sp,sp,#12
    str r0,[sp]
    str r1,[sp,#4]
    vmrs r2,fpscr
    str r2,[sp,#8]
    mov r2,#0x01000000
    vmsr fpscr,r2
    adr r12,2f
    vldmia r12!,{d0-d15}
    vldmia r12,{d16-d31}
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
    vstmia r0!,{d0-d15}
    vstmia r0,{d16-d31}
    ldr r0,[sp,#8]
    vmsr fpscr,r0
    add sp,sp,#12
    vpop {d8-d15}
    pop {r4-r11,pc}
.align 3
2:
.set native_ui_vfp_word,0x1000
.rept 64
    .word native_ui_vfp_word
    .set native_ui_vfp_word,native_ui_vfp_word+1
.endr
.size native_ui_check_frame,.-native_ui_check_frame
)");

int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    auto& host=GetNativeHost();assert(host.Initialize());
    using namespace bl2::ui;
    using namespace bl2::touch;
    const auto core=Dynarmic::A32::Jit::NativeCoreIdentity();
    Dynarmic::A32::Jit one,two;
    assert(&one!=&two && one.NativeCoreIdentity()==two.NativeCoreIdentity());
    std::uintptr_t other=0;
    std::thread thread([&] {other=Dynarmic::A32::Jit::NativeCoreIdentity();});thread.join();
    assert(other && other!=core);

    // Zero-origin reads address real native allocations; writes still stay
    // inside this transport's own scratch block.
    assert(host.GetGuestMemoryBase()==nullptr);
    GfxHostTransport transport(host,[] {return true;});
    const auto scratch=transport.scratch();assert(scratch);
    const std::uint32_t value=0x12345678;std::uint32_t readback=0;
    assert(transport.write(scratch,&value,4) && transport.read(scratch,&readback,4) && readback==value);
    assert(!transport.write(scratch-4,&value,4) && !transport.read(0,&readback,4));
    assert(!transport.write(scratch+GfxSession::kScratchBytes-2,&value,4));

    assert(!touchSettingsState.initialized && Bl2InterfaceScale()==1.25f && !Bl2ShowFps() && Bl2FrameRateLimit()==0);
    auto packet=std::vector<std::uint32_t>{kTouchSettingsVersion,0};
    packet.insert(packet.end(),touchSettingsState.values.begin(),touchSettingsState.values.end());
    assert(ExchangeBl2TouchSettings(packet)[1]==0 && !touchSettingsState.initialized);
    // Java publishes the first valid packet; zero-initialized native storage
    // is not a preference snapshot. Match the canonical protocol-test seed.
    for(std::size_t i=0;i<kTouchSettingCount;++i)
        packet[i+2]=i==0 ? 100u : i==17 ? 125u : touchSettingSpecs[i].toggle ? 0u :
            TouchFloatBits(static_cast<float>(touchSettingSpecs[i].minimum));
    packet[2+16]=0xFFFFFFFFu;
    auto oldPacket=packet;oldPacket[0]=2;oldPacket.resize(20);
    const auto oldResult=ExchangeBl2TouchSettings(oldPacket);
    assert(oldResult.size()==kTouchSettingCount+3 && oldResult[0]==kTouchSettingsVersion && oldResult[1]==0 && !touchSettingsState.initialized);
    const auto initial=ExchangeBl2TouchSettings(packet);
    assert(initial.size()==kTouchSettingCount+3 && initial[0]==kTouchSettingsVersion && initial[1]==1 && initial[3+17]==125 &&
        initial[3+7]==0 && initial[3+18]==0 && initial[3+19]==0 && Bl2InterfaceScale()==1.25f);
    assert(SetBl2TouchSetting(18,1) && SetBl2TouchSetting(19,60));
    packet[1]=initial[1]; // Stale Java acknowledgement cannot erase newer native FPS edits.
    const auto pendingFps=ExchangeBl2TouchSettings(packet);
    assert(pendingFps[1]>initial[1] && pendingFps[3+18]==1 && pendingFps[3+19]==60 && Bl2ShowFps() && Bl2FrameRateLimit()==60);
    packet[1]=pendingFps[1];packet[2+18]=1;packet[2+19]=90;
    const auto accepted=ExchangeBl2TouchSettings(packet);
    assert(accepted[3+17]==125 && accepted[3+18]==1 && accepted[3+19]==90 && Bl2FrameRateLimit()==90);
    packet[2+19]=45;
    assert(ExchangeBl2TouchSettings(packet)[3+19]==90 && !SetBl2TouchSetting(19,45));
    DebugBuildFooterRange footer;
    constexpr std::string_view buildHint="(X) Back up a step    (Y) Build this Gun";
    assert(debugBuildFooterRange("Gear Builder","BuildWeapon",buildHint,footer));
    assert(kDebugBuildFooterAction==Action::NextWeapon);
    assert(!debugBuildFooterRange("Dev Tools","BuildWeapon",buildHint,footer));
    assert(!debugBuildFooterRange("Gear Builder","",buildHint,footer));

    HudReapplyCall pending;
    HudLayoutPass pass;
    std::array<HudElementBaseline,7> baselines{};
    std::array<HudElementLayout,7> layouts{};
    for(auto& layout:layouts)layout={20,30,100,100};
    constexpr std::uint64_t owner=0x100000001ull,definition=0x200000002ull,generation=3;
    float applied=0;
    unsigned ticks=0,applications=0,returns=0;
    std::uint32_t application=0,returnEntry=0,bodyContinuation=0;
    auto& manager=host.GetThunkManager();
    const auto tickContinuation=host.CreateCodeVeneer(std::array<std::uint32_t,2>{0xE1A00000,0xE12FFF1E});
    assert(tickContinuation);
    returnEntry=manager.RegisterThunk("native_ui_scale_return",[&](Dynarmic::A32::Jit& frame,DynarmicHost&) {
        HudA32Registers restored;
        assert(pending.Finish(frame.NativeCoreIdentity(),frame.Regs()[13],owner,generation,restored));
        frame.Regs()=restored.regs;frame.ExtRegs()=restored.ext;
        frame.SetCpsr(restored.cpsr);frame.SetFpscr(restored.fpscr);++returns;
    });
    const auto body=manager.RegisterThunk("native_ui_original_apply",[&](Dynarmic::A32::Jit& frame,DynarmicHost&) {
        const HudLayoutFrame key{frame.NativeCoreIdentity(),owner,definition,generation,4,0x10000,7,frame.Regs()[13]};
        const float requested=Bl2InterfaceScale();assert(pass.Begin(key,requested));
        auto foreign=key;foreign.sp+=8;
        assert(!pass.Matches(foreign) && !pass.Finish(foreign,7,applied));
        foreign=key;++foreign.generation;assert(!pass.Matches(foreign));
        for(std::uint32_t i=0;i<7;++i) {
            assert(pass.Capture(key,i,100+i,200+i));
            assert(baselines[i].CaptureOrRestore(1,10+i,200+i,layouts[i]));
            layouts[i].scaleX*=requested;layouts[i].scaleY*=requested;
            assert(pass.CompleteElement(key,i,100+i,200+i));
        }
        assert(pass.Finish(key,7,applied) && applied==requested);
        frame.Regs()[0]=99;frame.Regs()[12]=100;frame.ExtRegs().fill(0xDEADBEEF);
        frame.SetCpsr(0xA0000000);frame.SetFpscr(0);frame.Regs()[15]=bodyContinuation;++applications;
    });
    // These exact stack/VFP instructions are the original ApplyElementScaling
    // prologue and epilogue. Its native return must reach the saved Tick stack.
    application=host.CreateCodeVeneer(std::array<std::uint32_t,9>{
        0xE92D4FF0,0xED2D8B04,0xE24DD0A4,0xE51FF004,body,
        0xE28DD0A4,0xECBD8B04,0xE8BD8FF0,0xE1A00000});
    bodyContinuation=application+20;
    const auto tick=manager.RegisterThunk("native_ui_scale_tick",[&](Dynarmic::A32::Jit& frame,DynarmicHost&) {
        const HudA32Registers saved{frame.Regs(),frame.ExtRegs(),frame.Cpsr(),frame.Fpscr()};
        assert(pending.Begin(frame.NativeCoreIdentity(),tickContinuation,saved,owner,generation));
        assert(!pending.Begin(frame.NativeCoreIdentity(),tickContinuation,saved,owner,generation));
        frame.Regs()[14]=returnEntry;frame.Regs()[15]=application;++ticks;
    });
    assert(returnEntry && body && application && tick);
    for(const auto percent:{125,200,100,130,100}) {
        assert(SetBl2TouchSetting(17,percent));
        std::array<std::uint32_t,79> out{};native_ui_check_frame(tick,out.data());
        for(std::size_t i=0;i<13;++i)assert(out[i]==i+1);
        assert((out[13]&0xF0000000u)==0x60000000u && out[14]==0x01000000u);
        for(std::size_t i=0;i<64;++i)assert(out[15+i]==0x1000+i);
        for(const auto& layout:layouts)assert(layout.scaleX==percent && layout.scaleY==percent);
        assert(!pending.core && !pass.frame.core);
    }
    assert(ticks==5 && applications==5 && returns==5);
    std::puts("PASS: native UI core/stack/lifetime, Tick->Apply->Return, all registers/VFP/flags, reversible 125/200/100, GFx identity mapping and original Gear Y");
}

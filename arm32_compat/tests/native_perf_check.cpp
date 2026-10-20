#include "native_perf.h"
#include <cstdio>
#include <cstring>
static bool spawnPawnCaptureCheck() {
    using bl2::native_perf::ClaimOriginalCheatAttempt;
    std::atomic<bool> attempted{false};
    unsigned invoked=0;
    const auto call=[&](bool requested,bool gameplay,bool armed,std::uint32_t token) {
        const bool claimed=ClaimOriginalCheatAttempt(attempted,requested,gameplay,armed,true,token);
        if (claimed) ++invoked;
        return claimed;
    };
    for (unsigned i=0;i<3;++i) if (call(true,true,true,0) || attempted.load()) return false;
    if (invoked || call(false,true,true,1901) || call(true,false,true,1901) ||
        call(true,true,false,1901) || attempted.load()) return false;
    if (!call(true,true,true,1901) || !attempted.load() || invoked!=1 ||
        call(true,true,true,1901) || call(true,true,true,1902) ||
        call(true,true,true,0) || invoked!=1) return false;
    // GiveAmmo retains its existing startup-once behavior with capture disabled.
    std::atomic<bool> ammoAttempted{false};
    if (!ClaimOriginalCheatAttempt(ammoAttempted,true,true,true,false,0) ||
        ClaimOriginalCheatAttempt(ammoAttempted,true,true,true,false,1901)) return false;
    std::puts("Native SpawnPawn capture controls PASS: token0 no attempt, token1901 one call, token1902 no retry; GiveAmmo unchanged");
    return true;
}
static bool spawnPawnSchemaCheck() {
    using namespace bl2::native_perf;
    const auto valid=[](std::uint32_t flags,unsigned count,unsigned bytes,
                       unsigned result,SpawnPawnArgument argument) {
        return SpawnPawnSignatureValid(flags,count,bytes,result,argument);
    };
    constexpr SpawnPawnArgument input{1,12,0,0x00400080};
    if (!valid(0x00820202,1,12,0xFFFF,input) ||
        valid(0x00820202,0,12,0xFFFF,input) ||
        valid(0x00820202,2,12,0xFFFF,input) ||
        valid(0x00820202,1,4,0xFFFF,input) ||
        valid(0x00820202,1,16,0xFFFF,input) ||
        valid(0x00820202,1,12,0,input) ||
        valid(0x00820202|0x00400000,1,12,0xFFFF,input) ||
        valid(0x00820202,1,12,0xFFFF,{0,12,0,input.flags}) ||
        valid(0x00820202,1,12,0xFFFF,{2,12,0,input.flags}) ||
        valid(0x00820202,1,12,0xFFFF,{1,4,0,input.flags}) ||
        valid(0x00820202,1,12,0xFFFF,{1,12,4,input.flags}) ||
        valid(0x00820202,1,12,0xFFFF,{1,12,0,0x80}) ||
        valid(0x00820202,1,12,0xFFFF,{1,12,0,0x00400180}) ||
        std::strcmp(kSpawnPawnTemplate,"GD_PrimalBeast.Character.Pawn_PrimalBeast") ||
        sizeof(kSpawnPawnTemplate)!=std::strlen(kSpawnPawnTemplate)+1) return false;
    std::puts("Native SpawnPawn original schema controls PASS (13), fixed FString includes NUL");
    return true;
}
static void dumpCatalogue() {
    using namespace bl2::native_perf;
    std::printf("[");
    for(std::size_t i=0;i<std::size(kEntries);++i) {
        const auto& entry=kEntries[i];
        const auto code=MakeReplay(entry,0x40000000u+entry.rva,0x10008000u);
        std::printf("%s{\"symbol\":\"%s\",\"rva\":%u,\"preimage\":[%u,%u],\"literal_rva\":%u,\"literal_word\":%u,\"kind\":%u,\"replay\":[",
            i?",":"",entry.symbol,entry.rva,entry.words[0],entry.words[1],entry.literalRva,entry.literalWord,unsigned(entry.replay));
        for(std::size_t j=0;j<code.count;++j)std::printf("%s%u",j?",":"",code.words[j]);
        std::printf("]}");
    }
    std::puts("]");
}
#if defined(BL2_PERF_CATALOGUE_ONLY)
int main(int argc,char** argv) {
    if(argc==2 && !std::strcmp(argv[1],"--spawn-schema"))
        return spawnPawnSchemaCheck() && spawnPawnCaptureCheck()?0:1;
    dumpCatalogue();
}
#else
#include "native_host.h"
#include "native_jni.h"
#include "thunk_manager.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <limits>
#include <pthread.h>

#if !defined(__arm__) || !BL2_NATIVE_PERF_PROBE
#error "Run this fixture on native ARM32 with BL2_NATIVE_PERF_PROBE enabled."
#endif
using namespace bl2::native_perf;
using Words = std::array<std::uint32_t,10>;

extern "C" void NativePerfCheckFrame(std::uint32_t entry,std::uint32_t* output);
extern "C" void NativePerfCheckClobber();
asm(R"(
.syntax unified
.arm
.fpu neon
.text
.align 2
.global NativePerfCheckFrame
.type NativePerfCheckFrame,%function
NativePerfCheckFrame:
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
    mov r12,#0x28000000
    orr r12,r12,#0x00050000
    msr APSR_nzcvqg,r12
    mov r12,#13
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
.size NativePerfCheckFrame,.-NativePerfCheckFrame
.global NativePerfCheckClobber
.type NativePerfCheckClobber,%function
NativePerfCheckClobber:
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
.size NativePerfCheckClobber,.-NativePerfCheckClobber
)");

static unsigned observed=0;
static std::uint32_t lastLr=0;
static std::uint32_t instrument(DynarmicHost& host,std::uint32_t site,Entry entry,
                                std::uint32_t forward=0) {
    const auto code=MakeReplay(entry,site,forward);
    assert(code.count);
    const auto replay=host.CreateCodeVeneer(std::span(code.words).first(code.count));
    assert(replay);
    const auto thunk=host.GetThunkManager().RegisterThunk("native_perf_check_"+std::to_string(site),
        [replay](Dynarmic::A32::Jit& frame,DynarmicHost&) {
            ++observed;lastLr=frame.Regs()[14];
            NativePerfCheckClobber();
            frame.Regs()[15]=replay;
        });
    assert(thunk);
    host.MemoryWrite32(site,0xE51FF004);host.MemoryWrite32(site+4,thunk);
    host.InvalidateGuestCode(site,8);
    return site;
}
static std::uint32_t branchTo(std::uint32_t site,std::uint32_t target,bool link) {
    const auto bytes=std::int64_t(target)-std::int64_t(site)-8;
    assert((bytes&3)==0 && bytes>=-33554432 && bytes<33554432);
    return (link?0xEB000000:0xEA000000)|(std::uint32_t(bytes/4)&0xFFFFFF);
}
static std::uint32_t nativeAddress(auto value) {
    return std::uint32_t(reinterpret_cast<std::uintptr_t>(value));
}
static void damageIdentityCheck(DynarmicHost& host) {
    const auto memory=host.GuestAlloc(0x10000,8);assert(memory);
    std::memset(reinterpret_cast<void*>(memory),0,0x10000);
    const IdentityGlobals globals{memory,memory+16,memory+32,memory+36};
    const auto objectData=memory+0x3000,nameData=memory+0x4000;
    unsigned count=0;
    const auto object=[&](std::uint32_t offset,const char* name,std::uint32_t cls) {
        const auto address=memory+offset,entry=memory+0x5000+count*144;
        std::strcpy(reinterpret_cast<char*>(entry+8),name);
        host.MemoryWrite32(objectData+count*4,address);
        host.MemoryWrite32(nameData+count*4,entry);
        host.MemoryWrite32(address+0x20,count);host.MemoryWrite32(address+0x2C,count++);
        host.MemoryWrite32(address+0x34,cls);return address;
    };
    const auto classClass=object(0x2000,"Class",memory+0x2000);
    const auto propertyClass=object(0x2080,"ObjectProperty",classClass);
    const auto controllerClass=object(0x2100,"Controller",classClass);
    const auto willowControllerClass=object(0x2180,"WillowPlayerController",classClass);
    const auto pawnClass=object(0x2200,"Pawn",classClass);
    const auto willowPawnClass=object(0x2280,"WillowPawn",classClass);
    const auto aiClass=object(0x2300,"WillowAIPawn",classClass);
    const auto playerClass=object(0x2380,"LocalPlayer",classClass);
    const auto worldClass=object(0x2400,"World",classClass);
    const auto engineClass=object(0x2480,"GameEngine",classClass);
    const auto property=object(0x2500,"Pawn",propertyClass);
    const auto engine=object(0x1000,"Engine",engineClass);
    const auto world=object(0x1600,"World",worldClass);
    const auto player=object(0x1680,"LocalPlayer",playerClass);
    const auto controller=object(0x1700,"Controller",willowControllerClass);
    const auto local=object(0x1800,"LocalPawn",willowPawnClass);
    const auto target=object(0x1880,"Enemy",aiClass);
    for (auto pair:{std::pair{globals.objects,objectData},std::pair{globals.names,nameData}}) {
        host.MemoryWrite32(pair.first,pair.second);host.MemoryWrite32(pair.first+4,count);
        host.MemoryWrite32(pair.first+8,count);
    }
    host.MemoryWrite32(willowControllerClass+0x40,controllerClass);
    host.MemoryWrite32(willowPawnClass+0x40,pawnClass);host.MemoryWrite32(aiClass+0x40,willowPawnClass);
    host.MemoryWrite32(controllerClass+0x44,property);
    host.MemoryWrite32(controllerClass+0x48,0x100);host.MemoryWrite32(willowControllerClass+0x48,0x100);
    host.MemoryWrite32(property+0x28,controllerClass);host.MemoryWrite32(property+0x40,1);
    host.MemoryWrite32(property+0x44,4);host.MemoryWrite32(property+0x54,0x80);
    host.MemoryWrite32(property+0x74,pawnClass);
    host.MemoryWrite32(globals.engine,engine);host.MemoryWrite32(globals.world,world);
    host.MemoryWrite32(engine+0x48C,memory+64);host.MemoryWrite32(engine+0x490,1);
    host.MemoryWrite32(engine+0x494,1);host.MemoryWrite32(memory+64,player);
    host.MemoryWrite32(player+0x40,controller);host.MemoryWrite32(controller+0x80,local);
    host.MemoryWrite32(target+0x30,2);
    const auto observed=ReadDamageIdentity(host,globals,target);
    assert(observed.targetKnown && observed.localKnown && observed.localPawn==local);
    assert(observed.targetToken && observed.localToken && observed.controller==controller && observed.world==world);
    assert(!std::strcmp(observed.targetName,"Enemy_1") && !std::strcmp(observed.targetClass,"WillowAIPawn"));
    const auto own=ReadDamageIdentity(host,globals,local);
    assert(own.targetKnown && own.localKnown && own.localPawn==local);
    for (auto pair:{std::pair{property+0x40,2u},std::pair{property+0x44,8u},
                   std::pair{property+0x54,0x100u},std::pair{property+0x74,aiClass},
                   std::pair{property+0x28,willowControllerClass},std::pair{controller+0x80,0u},
                   std::pair{engine+0x490,2u},std::pair{player+0x34,aiClass}}) {
        const auto before=host.MemoryRead32(pair.first);host.MemoryWrite32(pair.first,pair.second);
        const auto bad=ReadDamageIdentity(host,globals,target);
        assert(bad.targetKnown && !bad.localKnown);
        host.MemoryWrite32(pair.first,before);
    }
    host.MemoryWrite32(target+0x0C,0x20000002);assert(!ReadDamageIdentity(host,globals,target).targetKnown);
    host.MemoryWrite32(target+0x0C,0);assert(!ReadDamageIdentity(host,globals,0).targetKnown);
    host.MemoryWrite32(globals.objects+4,count+1);assert(!ReadDamageIdentity(host,globals,target).targetKnown);
    host.MemoryWrite32(globals.objects+4,count);
    host.GuestFree(memory);
    std::puts("PASS: passive damaged/local Pawn reflection, numbered identity and 11 schema/liveness/array negatives");
}
static void mutexFunction(DynarmicHost& host,Entry entry,Words words,
                          std::uint32_t data,std::uint32_t nativeCall,std::size_t callIndex) {
    // Keep all original instructions, including the real mutex and lock byte.
    // Relocate only the PIC data literal and the native PLT call for this copy.
    const std::array shim{0xE59FC000u,0xE12FFF1Cu,nativeCall};
    const auto callee=host.CreateCodeVeneer(shim);assert(callee);
    const auto site=host.CreateCodeVeneer(words);assert(site);
    words[9]=data-(site+16);
    words[callIndex]=branchTo(site+std::uint32_t(callIndex*4),callee,true);
    for (std::size_t i=0;i<words.size();++i)host.MemoryWrite32(site+std::uint32_t(i*4),words[i]);
    host.InvalidateGuestCode(site,sizeof(words));
    const auto expected=host.RunGuestCallback(site,{});
    if (entry.rva==0x02BA81F0) {
        assert(expected==0 && host.MemoryRead8(data+0x208)==1);
        assert(pthread_mutex_unlock(reinterpret_cast<pthread_mutex_t*>(data+0x20C))==0);
    } else {
        assert(expected==0 && host.MemoryRead8(data+0x208)==0);
        assert(pthread_mutex_lock(reinterpret_cast<pthread_mutex_t*>(data+0x20C))==0);
    }
    entry.literalWord=words[9];
    instrument(host,site,entry);
    assert(host.RunGuestCallback(site,{})==expected && lastLr);
}
int main(int argc,char** argv) {
    if(argc==2 && !std::strcmp(argv[1],"--catalogue")) { dumpCatalogue(); return 0; }
    assert(spawnPawnSchemaCheck());
    assert(spawnPawnCaptureCheck());
    std::setvbuf(stdout,nullptr,_IONBF,0);
    auto& host=GetNativeHost();assert(host.Initialize());
    assert(std::size(kEntries)==30 && kNvsiEntries==28);
    assert(NativeFrameRateFor(59.94f,0)==59.94f);
    for(auto display:{30.f,60.f,90.f,120.f})
        for(auto cap:{30u,60u,90u})assert(NativeFrameRateFor(display,cap)==float(cap));
    assert(NativeFrameRateFor(90,45)==0 && NativeFrameRateFor(0,30)==0);
    assert(NativeFrameRateFor(std::numeric_limits<float>::quiet_NaN(),0)==0);
    std::puts("PASS: display/fractional modes and explicit 30/60/90 policy, invalid input rejection");
    damageIdentityCheck(host);

    const auto license=host.CreateCodeVeneer(kEntries[0].words);
    assert(license);instrument(host,license,kEntries[0]);
    std::array<std::uint32_t,25> frame{};
    NativePerfCheckFrame(license,frame.data());
    for(std::size_t i=0;i<13;++i)assert(frame[i]==i+1);
    assert((frame[13]&0xF80F0000u)==0x28050000u && frame[14]==0);
    for(std::size_t i=15;i<frame.size();++i)assert(frame[i]==i-14);
    std::puts("PASS: observer preserves live R0..R12, APSR, FPSCR and VFP across original license BX LR");
    for(auto index:{13u,17u}) {
        const auto site=host.CreateCodeVeneer(kEntries[index].words);assert(site);
        assert(host.RunGuestCallback(site,{7})==0);
        instrument(host,site,kEntries[index]);assert(host.RunGuestCallback(site,{7})==0);
    }
    const std::array<std::uint32_t,12> ammoWords{
        0xE5D01008,0xE3510000,0x1590100C,0x1A000004,0xE5D01010,0xE3510000,
        0x15901018,0x1A000000,0xE3A01000,0xE580101C,0xE3A00000,0xE12FFF1E};
    const auto ammo=host.CreateCodeVeneer(ammoWords);assert(ammo);
    std::array<std::uint32_t,8> object{};
    object[3]=123;object[6]=456;
    for(unsigned which=0;which<3;++which) {
        object[2]=which==1;object[4]=which==2;object[7]=999;
        assert(host.RunGuestCallback(ammo,{nativeAddress(object.data())})==0);
        assert(object[7]==(which==1?123u:which==2?456u:0u));
    }
    instrument(host,ammo,kEntries[23]);
    for(unsigned which=0;which<3;++which) {
        object[2]=which==1;object[4]=which==2;object[7]=999;
        assert(host.RunGuestCallback(ammo,{nativeAddress(object.data())})==0);
        assert(object[7]==(which==1?123u:which==2?456u:0u));
    }
    std::puts("PASS: unchanged original trajectory/slope return and all three Ammo CPU branches");

    alignas(16) std::array<std::uint8_t,0x210+sizeof(pthread_mutex_t)> nvsi{};
    auto* mutex=reinterpret_cast<pthread_mutex_t*>(nvsi.data()+0x20C);
    assert(pthread_mutex_init(mutex,nullptr)==0);
    mutexFunction(host,kEntries[2],{0xE92D4010,0xE59F4018,0xE08F4004,0xE2840F83,0xEB8B6648,
        0xE3A03001,0xE3A00000,0xE5C43208,0xE8BD8010,0x004E0690},nativeAddress(nvsi.data()),nativeAddress(pthread_mutex_lock),4);
    assert(nvsi[0x208]==1);
    mutexFunction(host,kEntries[4],{0xE59F301C,0xE92D4010,0xE08F3003,0xE3A04000,0xE2830F83,
        0xE5C34208,0xEB8B663F,0xE1A00004,0xE8BD8010,0x004E0668},nativeAddress(nvsi.data()),nativeAddress(pthread_mutex_unlock),6);
    assert(nvsi[0x208]==0 && pthread_mutex_destroy(mutex)==0);
    std::puts("PASS: original Lock/Unlock instructions use a real pthread mutex and preserve return/lock byte");

    for(auto index:{1u,3u,5u,7u,9u,11u}) {
        auto entry=kEntries[index];
        const auto literalIndex=entry.replay==Replay::Literal0?0u:1u;
        const auto reg=(entry.words[literalIndex]>>12)&15;
        const auto push=entry.words[1-literalIndex];assert((push&0xFFFF0000u)==0xE92D0000u);
        const auto literalSlot=(literalIndex*4+8+(entry.words[literalIndex]&4095))/4;
        std::array<std::uint32_t,30> code{};code[0]=entry.words[0];code[1]=entry.words[1];
        code[2]=0xE1A00000u|reg;code[3]=0xE8BD0000u|((push&0xFFFFu)&~0x4000u)|0x8000u;
        code[literalSlot]=0x12345678u;entry.literalWord=code[literalSlot];
        const auto site=host.CreateCodeVeneer(code);assert(site);
        assert(host.RunGuestCallback(site,{})==0x12345678u);
        instrument(host,site,entry);assert(host.RunGuestCallback(site,{})==0x12345678u);
    }
    const auto force=host.CreateCodeVeneer(std::array{0xE59F3008u,0xE08F3003u,0xE1A00003u,0xE12FFF1Eu,0u});assert(force);
    auto forceEntry=kEntries[16];forceEntry.literalWord=0x12345678u-(force+12);
    host.MemoryWrite32(force+16,forceEntry.literalWord);host.InvalidateGuestCode(force,20);
    assert(host.RunGuestCallback(force,{})==0x12345678u);
    instrument(host,force,forceEntry);assert(host.RunGuestCallback(force,{})==0x12345678u);
    const auto target=host.CreateCodeVeneer(std::array{0xE1A00001u,0xE12FFF1Eu});assert(target);
    const auto init=host.CreateCodeVeneer(std::array{0xE3A01000u,0u});assert(init);
    auto initEntry=kEntries[27];initEntry.words[1]=branchTo(init+4,target,false);
    host.MemoryWrite32(init+4,initEntry.words[1]);host.InvalidateGuestCode(init,8);
    assert(host.RunGuestCallback(init,{7,9})==0);
    instrument(host,init,initEntry);assert(host.RunGuestCallback(init,{7,9})==0);
    const auto local=host.GetThunkManager().RegisterThunk("native_perf_check_lifecycle",
        [](Dynarmic::A32::Jit& frame,DynarmicHost&){frame.Regs()[0]=4;});assert(local);
    const auto forward=host.CreateCodeVeneer(std::array{0xE51FF004u,local});assert(forward);
    assert(host.RunGuestCallback(forward,{7})==4);
    instrument(host,forward,kEntries[10],local);assert(host.RunGuestCallback(forward,{7})==4);
    std::puts("PASS: original literal/PIC ADD, LR-preserving tail branch and existing lifecycle continuation");
    assert(observed>=16 && lastLr);
    std::printf("PASS: native entry observers executed %u times; no engine/save/device APIs invoked\n",observed);
}
#endif

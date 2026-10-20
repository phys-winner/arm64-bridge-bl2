#include "native_perf.h"
#include "native_host.h"
#include "thunk_manager.h"
#include "bl2_touch_input.h"
#include "bl2_touch_runtime.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if BL2_NATIVE_PERF_PROBE
namespace {
using namespace bl2::native_perf;
constexpr std::size_t kCount = std::size(kEntries), kCallerSlots = 8;
constexpr const char* kTag = "BL2NativePerf";
struct Caller { std::atomic<std::uint32_t> lr{0}, calls{0}; };
struct Counter {
    std::atomic<std::uint32_t> calls{0}, droppedCallers{0};
    std::array<Caller,kCallerSlots> callers{};
};
std::array<Counter,kCount> counters;
std::uint32_t imageBase = 0, frameCounter = 0;
std::atomic<bool> installed{false};
bool giveAmmoRequested=false;
std::atomic<bool> giveAmmoAttempted{false};
bool spawnPawnRequested=false;
std::atomic<bool> spawnPawnAttempted{false};
std::atomic<std::uint32_t> damagePawn{0}, damageSummary{0}, damageLr{0};
struct Boundary {
    std::uint32_t capture = 0, sequence = 0, framesLogged = 0, slowLogged = 0, summaries = 0;
    std::uint64_t wall = 0, cpu = 0, process = 0, summaryWall = 0;
    std::array<std::uint32_t,kCount> processCalls{}, threadCalls{}, summaryCalls{}, dropped{};
    std::array<std::array<std::uint32_t,kCallerSlots>,kCount> callerCalls{};
};
struct ThreadState {
    std::array<std::uint32_t,kCount> calls{};
    Boundary tick{}, present{};
    std::uint64_t lastPoll = 0;
    std::uint32_t capture = 0;
    std::uint32_t damageCapture = 0, lastDamagePawn = 0, damageLogs = 0;
};
thread_local ThreadState thread;

void originalCheatOnce(DynarmicHost& host,const bl2::touch::Snapshot& context,bool spawn) {
    const bool requested=spawn?spawnPawnRequested:giveAmmoRequested;
    auto& attempted=spawn?spawnPawnAttempted:giveAmmoAttempted;
    const char* command=spawn?"SpawnPawn":"GiveAmmo";
    if (!ClaimOriginalCheatAttempt(attempted,requested,
            context.context.mode==bl2::touch::Mode::Gameplay,
            context.context.weaponCount>=1,spawn,thread.capture)) return;
    // These are the only diagnostic commands. SpawnPawn receives a fixed stock
    // archetype; neither command can evaluate user strings or set level/HP/AI.
    const auto generation=Bl2TouchLifetimeGeneration();
    const auto span=[](std::uint32_t p,std::size_t n) { return p>=0x10000 && std::uint64_t(p)+n<=UINT32_MAX; };
    const auto read=[&](std::uint32_t p) { return span(p,4)?host.MemoryRead32(p):0; };
    const auto array=[&](std::uint32_t p,std::uint32_t limit,std::uint32_t& data,std::uint32_t& count) {
        if (!span(p,12)) return false;
        data=read(p);count=read(p+4);const auto capacity=read(p+8);
        return count<=capacity && capacity<=limit && (!count || span(data,std::size_t(count)*4));
    };
    std::uint32_t objects=0,objectCount=0,names=0,nameCount=0;
    const auto reject=[&](const char* why) {
        __android_log_print(ANDROID_LOG_WARN,kTag,"NATIVE_CHEAT rejected name=%s reason=%s",command,why);
    };
    if (!array(imageBase+0x02FBA268,2000000,objects,objectCount) ||
        !array(imageBase+0x02FB00C8,2000000,names,nameCount)) { reject("global-arrays"); return; }
    const auto valid=[&](std::uint32_t object) {
        if (!span(object,0x38) || (read(object+0x0C)&0x20000002u)) return false;
        const auto index=read(object+0x20);
        return index<objectCount && read(objects+index*4)==object;
    };
    const auto named=[&](std::uint32_t object,const char* expected) {
        if (!valid(object) || read(object+0x30)) return false;
        const auto index=read(object+0x2C);
        if (index>=nameCount) return false;
        const auto text=read(names+index*4)+8;
        if (!span(text,128)) return false;
        for (unsigned i=0;i<128;++i) {
            const auto actual=host.MemoryRead8(text+i);
            if (actual!=static_cast<unsigned char>(expected[i])) return false;
            if (!actual) return true;
        }
        return false;
    };
    const auto isA=[&](std::uint32_t object,const char* typeName) {
        if (!valid(object)) return false;
        auto type=read(object+0x34);
        for (unsigned i=0;type && i<48;++i) {
            if (!valid(type) || !span(type,0x44)) return false;
            if (named(type,typeName)) return true;
            type=read(type+0x40);
        }
        return false;
    };
    const auto engine=read(imageBase+0x03008D20),world=read(imageBase+0x0300D940);
    std::uint32_t players=0,playerCount=0;
    if (!valid(engine) || !valid(world) || !array(engine+0x48C,4,players,playerCount) || playerCount!=1) {
        reject("single-local-player");return;
    }
    const auto local=read(players),controller=valid(local)?read(local+0x40):0;
    if (!isA(controller,"WillowPlayerController")) { reject("controller-type");return; }
    std::uint32_t cheat=0,archetype=0;
    for (std::uint32_t i=0;i<objectCount;++i) {
        const auto candidate=read(objects+i*4);
        if (spawn && named(candidate,"Pawn_PrimalBeast") && isA(candidate,"WillowAIPawn")) {
            const auto group=read(candidate+0x28),package=valid(group)?read(group+0x28):0;
            if (named(group,"Character") && named(package,"GD_PrimalBeast") && !read(package+0x28)) {
                if (archetype) { reject("ambiguous-stock-archetype");return; }
                archetype=candidate;
            }
        }
        if (!valid(candidate) || read(candidate+0x28)!=controller || !isA(candidate,"WillowCheatManager")) continue;
        if (cheat) { reject("ambiguous-owned-cheat-manager");return; }
        cheat=candidate;
    }
    std::uint32_t fn=0;
    auto type=valid(cheat)?read(cheat+0x34):0;
    for (unsigned depth=0;type && depth<48 && !fn;++depth) {
        if (!valid(type) || !span(type,0x48)) break;
        auto field=read(type+0x44);
        for (unsigned n=0;field && n<2048;++n) {
            if (!valid(field) || read(field+0x28)!=type) break;
            if (named(field,command) && named(type,"WillowCheatManager") &&
                named(read(field+0x34),"Function")) { fn=field;break; }
            field=read(field+0x3C);
        }
        type=read(type+0x40);
    }
    if (!fn || !span(fn,0xA0) || read(fn+0x7C)!=0x00820202 ||
        host.MemoryRead8(fn+0x85)!=(spawn?1:0) || host.MemoryRead16(fn+0x86)!=(spawn?12:0) ||
        host.MemoryRead16(fn+0x88)!=0xFFFF || read(read(cheat)+0x10C)!=imageBase+0x010E826C) {
        reject("original-command-schema");return;
    }
    std::uint32_t argument=0;
    DamageIdentity localIdentity{};
    if (spawn) {
        if (!archetype) { reject("stock-archetype-not-loaded");return; }
        auto field=read(fn+0x44);
        for (unsigned i=0;field && i<16;++i) {
            if (!valid(field) || !span(field,0x68) || read(field+0x28)!=fn) { reject("argument-chain");return; }
            if (host.MemoryRead64(field+0x48)&0x80) {
                if (argument || !named(field,"args") || !named(read(field+0x34),"StrProperty") ||
                    !SpawnPawnSignatureValid(read(fn+0x7C),host.MemoryRead8(fn+0x85),
                        host.MemoryRead16(fn+0x86),host.MemoryRead16(fn+0x88),
                        {read(field+0x40),read(field+0x44),read(field+0x54),host.MemoryRead64(field+0x48)})) {
                    reject("original-FString-schema");return;
                }
                argument=field;
            }
            field=read(field+0x3C);
        }
        if (field || !argument) { reject("argument-chain");return; }
        localIdentity=ReadDamageIdentity(host,{imageBase+0x02FBA268,imageBase+0x02FB00C8,
            imageBase+0x03008D20,imageBase+0x0300D940},archetype);
        if (!localIdentity.targetKnown || !localIdentity.localKnown || localIdentity.controller!=controller ||
            localIdentity.world!=world) { reject("original-local-Pawn");return; }
    }
    if (spawn) {
        const auto current=ReadDamageIdentity(host,{imageBase+0x02FBA268,imageBase+0x02FB00C8,
            imageBase+0x03008D20,imageBase+0x0300D940},archetype);
        if (!current.targetKnown || !current.localKnown || current.controller!=controller || current.world!=world ||
            current.targetToken!=localIdentity.targetToken || current.localToken!=localIdentity.localToken) {
            reject("changed-local-Pawn");return;
        }
    }
    const auto fresh=bl2::touch::sharedMailbox().snapshot();
    if (generation!=Bl2TouchLifetimeGeneration() || fresh.epoch!=context.epoch ||
        fresh.context.sceneIdentity!=context.context.sceneIdentity ||
        fresh.context.mode!=bl2::touch::Mode::Gameplay || !valid(controller) || !valid(cheat) || !valid(fn) ||
        read(imageBase+0x0300D940)!=world || read(local+0x40)!=controller || read(cheat+0x28)!=controller ||
        (spawn && (!valid(archetype) || !valid(argument) || !valid(localIdentity.localPawn)))) {
        reject("changed-lifetime");return;
    }
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "NATIVE_CHEAT invoke name=%s controller=%08X manager=%08X fn=%08X diagnostic_only=1",command,controller,cheat,fn);
    if (!spawn) host.RunGuestCallback(imageBase+0x010E826C,{cheat,fn,0,0});
    else {
        // Original ProcessEvent copies this 12-byte input frame to its locals;
        // its cleanup does not destroy parameters below ParmsSize. This fixed
        // no-out FString and mutable text remain caller-owned for this call.
        std::array<char,sizeof(kSpawnPawnTemplate)> text{};
        std::memcpy(text.data(),kSpawnPawnTemplate,text.size());
        SpawnPawnFrame frame{std::uint32_t(reinterpret_cast<std::uintptr_t>(text.data())),
            std::int32_t(text.size()),std::int32_t(text.size())};
        host.RunGuestCallback(imageBase+0x010E826C,
            {cheat,fn,std::uint32_t(reinterpret_cast<std::uintptr_t>(&frame)),0});
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "NATIVE_CHEAT completed name=SpawnPawn template=%s archetype=%08X local=%08X "
            "result=original_void_command spawn_success=unconfirmed",kSpawnPawnTemplate,archetype,localIdentity.localPawn);
    }
}

std::uint64_t clockNs(clockid_t clock) {
    timespec value{};
    if (clock_gettime(clock,&value)) return 0;
    return std::uint64_t(value.tv_sec)*1'000'000'000ULL+std::uint64_t(value.tv_nsec);
}
bool enabled() {
    char value[PROP_VALUE_MAX]{};
    return __system_property_get("debug.bl2.native_perf",value)>0 && !std::strcmp(value,"1");
}
std::uint32_t captureAt(std::uint64_t now) {
    if (!thread.lastPoll || now-thread.lastPoll>=1'000'000'000ULL) {
        thread.lastPoll=now;
        char value[PROP_VALUE_MAX]{};
        char* end=nullptr;
        const auto count=__system_property_get("debug.bl2.native_perf_capture",value);
        const auto token=count>0?std::strtoul(value,&end,10):0;
        thread.capture=count>0 && end!=value && *end==0 && token<=UINT32_MAX
            ?std::uint32_t(token):0;
    }
    return thread.capture;
}
void describeDamage(DynarmicHost& host,std::uint32_t pawn,std::uint32_t summary,std::uint32_t lr) {
    const auto capture=captureAt(clockNs(CLOCK_MONOTONIC));
    if (!capture) return;
    if (thread.damageCapture!=capture) {
        thread.damageCapture=capture;thread.lastDamagePawn=0;thread.damageLogs=0;
    }
    if (thread.lastDamagePawn==pawn || thread.damageLogs>=16) return;
    thread.lastDamagePawn=pawn;++thread.damageLogs;
    const auto identity=ReadDamageIdentity(host,{imageBase+0x02FBA268,imageBase+0x02FB00C8,
        imageBase+0x03008D20,imageBase+0x0300D940},pawn);
    const auto role=identity.targetKnown && identity.localKnown
        ?(pawn==identity.localPawn?"local":"non_local"):"unknown";
    const auto inImage=lr>=imageBase && std::uint64_t(lr)<std::uint64_t(imageBase)+0x03089000;
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "NATIVE_DAMAGE_ID token=%u tid=%d role=%s target=%08X target_token=%016llX name=%s class=%s target_known=%u local=%08X local_token=%016llX local_known=%u controller=%08X world=%08X summary=%08X %s=%08X reason=%s",
        capture,gettid(),role,pawn,static_cast<unsigned long long>(identity.targetToken),
        identity.targetName,identity.targetClass,unsigned(identity.targetKnown),identity.localPawn,
        static_cast<unsigned long long>(identity.localToken),unsigned(identity.localKnown),
        identity.controller,identity.world,summary,inImage?"caller_rva":"caller_ptr",inImage?lr-imageBase:lr,identity.reason);
}
void observe(std::size_t index, std::uint32_t lr) {
    ++thread.calls[index];
    auto& counter=counters[index];
    counter.calls.fetch_add(1,std::memory_order_relaxed);
    for (auto& caller:counter.callers) {
        auto key=caller.lr.load(std::memory_order_relaxed);
        if (key==0) {
            if (caller.lr.compare_exchange_strong(key,lr,std::memory_order_relaxed)) key=lr;
        }
        if (key==lr) { caller.calls.fetch_add(1,std::memory_order_relaxed); return; }
    }
    counter.droppedCallers.fetch_add(1,std::memory_order_relaxed);
}
void sampleCounters(Boundary& state) {
    for (std::size_t i=0;i<kCount;++i) {
        state.processCalls[i]=state.summaryCalls[i]=counters[i].calls.load(std::memory_order_relaxed);
        state.threadCalls[i]=thread.calls[i];
        state.dropped[i]=counters[i].droppedCallers.load(std::memory_order_relaxed);
        for (std::size_t j=0;j<kCallerSlots;++j)
            state.callerCalls[i][j]=counters[i].callers[j].calls.load(std::memory_order_relaxed);
    }
}
void boundary(DynarmicHost& host, Boundary& state, const char* kind,
              std::uint64_t swapNs=0, bool succeeded=true) {
    const auto wall=clockNs(CLOCK_MONOTONIC);
    const auto capture=captureAt(wall);
    if (!capture) { state.capture=0; return; }
    const auto cpu=clockNs(CLOCK_THREAD_CPUTIME_ID), process=clockNs(CLOCK_PROCESS_CPUTIME_ID);
    if (state.capture!=capture) {
        state={}; state.capture=capture;
        state.wall=state.summaryWall=wall; state.cpu=cpu; state.process=process;
        sampleCounters(state);
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "NATIVE_CAPTURE token=%u boundary=%s tid=%d clock=MONOTONIC counts=executed-entry slots=%zu",
            capture,kind,gettid(),kCallerSlots);
        return;
    }
    ++state.sequence;
    const auto context=bl2::touch::sharedMailbox().snapshot();
    const auto mode=unsigned(context.context.mode);
    // A Tick interval includes the engine's sleep/cap and observer overhead.
    // It is not the time spent solely inside FEngineLoop::Tick.
    char counts[768]{};
    std::size_t used=0;
    std::uint32_t nvsiProcess=0,nvsiThread=0,damage=0;
    for (std::size_t i=0;i<kCount;++i) {
        const auto total=counters[i].calls.load(std::memory_order_relaxed);
        const auto pd=total-state.processCalls[i],td=thread.calls[i]-state.threadCalls[i];
        state.processCalls[i]=total; state.threadCalls[i]=thread.calls[i];
        if (i<kNvsiEntries) { nvsiProcess+=pd; nvsiThread+=td; }
        if (i==kDamageEntry) damage=pd;
        if (pd && used<sizeof(counts)-1) {
            const int n=std::snprintf(counts+used,sizeof(counts)-used," %s=%u/%u",kEntries[i].label,td,pd);
            if (n>0) used+=std::size_t(n)<sizeof(counts)-used?std::size_t(n):sizeof(counts)-used-1;
        }
    }
    const auto elapsed=wall-state.wall;
    const bool gameplay=mode==unsigned(bl2::touch::Mode::Gameplay) || mode==unsigned(bl2::touch::Mode::Vehicle);
    if (gameplay && (state.framesLogged<30 || (elapsed>=50'000'000 && state.slowLogged<12))) {
        if (state.framesLogged<30) ++state.framesLogged; else ++state.slowLogged;
        const auto hitLr=damageLr.load(std::memory_order_relaxed);
        const auto hitRva=hitLr>=imageBase && std::uint64_t(hitLr)<std::uint64_t(imageBase)+0x03089000?hitLr-imageBase:0;
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "NATIVE_FRAME token=%u boundary=%s tid=%d seq=%u mode=%u epoch=%llu gframe=%llu wall_ms=%.3f thread_cpu_ms=%.3f process_cpu_ms=%.3f swap_ms=%.3f swap_ok=%u nvsi_thread=%u nvsi_process=%u damage=%u pawn=%08X damage_lr=%08X |%s",
            capture,kind,gettid(),state.sequence,mode,static_cast<unsigned long long>(context.epoch),
            static_cast<unsigned long long>(host.MemoryRead64(frameCounter)),double(elapsed)/1e6,
            double(cpu-state.cpu)/1e6,double(process-state.process)/1e6,double(swapNs)/1e6,unsigned(succeeded),
            nvsiThread,nvsiProcess,damage,damagePawn.load(std::memory_order_relaxed),
            hitRva,counts);
    }
    state.wall=wall; state.cpu=cpu; state.process=process;
    if (gameplay && wall-state.summaryWall>=1'000'000'000 && state.summaries<32) {
        ++state.summaries;
        __android_log_print(ANDROID_LOG_INFO,kTag,
            "NATIVE_SUMMARY token=%u boundary=%s tid=%d n=%u seq=%u elapsed_ms=%.3f mode=%u gframe=%llu pawn=%08X damage_summary=%08X",
            capture,kind,gettid(),state.summaries,state.sequence,double(wall-state.summaryWall)/1e6,mode,
            static_cast<unsigned long long>(host.MemoryRead64(frameCounter)),
            damagePawn.load(std::memory_order_relaxed),damageSummary.load(std::memory_order_relaxed));
        state.summaryWall=wall;
        for (std::size_t i=0;i<kCount;++i) {
            const auto total=counters[i].calls.load(std::memory_order_relaxed);
            const auto delta=total-state.summaryCalls[i]; state.summaryCalls[i]=total;
            if (!delta) continue;
            char callers[320]{}; used=0;
            for (std::size_t j=0;j<kCallerSlots;++j) {
                const auto count=counters[i].callers[j].calls.load(std::memory_order_relaxed);
                const auto dc=count-state.callerCalls[i][j];state.callerCalls[i][j]=count;
                if (!dc || used>=sizeof(callers)-1) continue;
                const auto lr=counters[i].callers[j].lr.load(std::memory_order_relaxed);
                const bool inImage=lr>=imageBase && std::uint64_t(lr)<std::uint64_t(imageBase)+0x03089000;
                const int n=std::snprintf(callers+used,sizeof(callers)-used," %s%08X/%u",inImage?"rva:":"ptr:",inImage?lr-imageBase:lr,dc);
                if (n>0) used+=std::size_t(n)<sizeof(callers)-used?std::size_t(n):sizeof(callers)-used-1;
            }
            const auto dropped=counters[i].droppedCallers.load(std::memory_order_relaxed);
            const auto dd=dropped-state.dropped[i];state.dropped[i]=dropped;
            __android_log_print(ANDROID_LOG_INFO,kTag,
                "NATIVE_CALLS token=%u boundary=%s tid=%d entry=%s calls=%u caller_overflow=%u |%s",
                capture,kind,gettid(),kEntries[i].label,delta,dd,callers);
        }
    }
}
} // namespace

bl2::native_perf::DamageIdentity bl2::native_perf::ReadDamageIdentity(
        DynarmicHost& host,IdentityGlobals globals,std::uint32_t target) {
    DamageIdentity result{};
    const auto generation=Bl2TouchLifetimeGeneration();
    const auto span=[&](std::uint32_t p,std::size_t n) {
        return p>=0x10000 && p<=host.GetGuestMemorySize() && n<=host.GetGuestMemorySize()-p;
    };
    const auto read=[&](std::uint32_t p) { return span(p,4)?host.MemoryRead32(p):0; };
    const auto array=[&](std::uint32_t p,std::uint32_t limit,std::uint32_t& data,std::uint32_t& count) {
        if (!span(p,12)) return false;
        data=read(p);count=read(p+4);const auto capacity=read(p+8);
        return count<=capacity && capacity<=limit && (!count || span(data,std::size_t(count)*4));
    };
    std::uint32_t objects=0,objectCount=0,names=0,nameCount=0;
    if (!array(globals.objects,2000000,objects,objectCount) ||
        !array(globals.names,2000000,names,nameCount)) { result.reason="global-arrays";return result; }
    const auto token=[&](std::uint32_t object)->std::uint64_t {
        if (!span(object,0x38) || (read(object+0x0C)&0x20000002u)) return 0;
        const auto index=read(object+0x20);
        return index<objectCount && read(objects+index*4)==object
            ?(std::uint64_t(object)<<32)|index:0;
    };
    const auto name=[&](std::uint32_t object,char* out) {
        if (!token(object) || read(object+0x30)) return false;
        const auto index=read(object+0x2C);
        if (index>=nameCount) return false;
        const auto entry=read(names+index*4);
        if (!span(entry,136)) return false;
        for (unsigned i=0;i<128;++i) {
            const auto c=host.MemoryRead8(entry+8+i);
            if (!c) { out[i]=0;return true; }
            if (c<32 || c>126 || i==127) return false;
            out[i]=static_cast<char>(c);
        }
        return false;
    };
    const auto named=[&](std::uint32_t object,const char* expected) {
        char text[128]{};return name(object,text) && !std::strcmp(text,expected);
    };
    const auto isA=[&](std::uint32_t object,const char* expected) {
        if (!token(object)) return false;
        auto type=read(object+0x34);
        for (unsigned i=0;type && i<48;++i) {
            if (!token(type) || !span(type,0x44) || !named(read(type+0x34),"Class")) return false;
            if (named(type,expected)) return true;
            type=read(type+0x40);
        }
        return false;
    };
    result.targetToken=token(target);
    // The instance's numbered FName is printed as its exact index name plus
    // the raw token, rather than rejecting a legitimate spawned actor suffix.
    if (result.targetToken && isA(target,"WillowPawn")) {
        const auto targetType=read(target+0x34);
        result.targetKnown=name(targetType,result.targetClass);
        const auto number=read(target+0x30);
        if (!number) name(target,result.targetName);
        else {
            const auto index=read(target+0x2C);
            if (index<nameCount) {
                const auto entry=read(names+index*4);
                if (span(entry,136)) {
                    char text[96]{};unsigned i=0;
                    for (;i<95;++i) {
                        const auto c=host.MemoryRead8(entry+8+i);
                        if (!c) break;
                        if (c<32 || c>126) { i=95;break; }
                        text[i]=static_cast<char>(c);
                    }
                    if (i<95) std::snprintf(result.targetName,sizeof(result.targetName),"%s_%u",text,number-1);
                }
            }
        }
    }
    result.reason="target-unknown";
    if (!result.targetKnown) return result;
    const auto engine=read(globals.engine);
    result.world=read(globals.world);
    std::uint32_t players=0,playerCount=0;
    const auto engineToken=token(engine),worldToken=token(result.world);
    if (!engineToken || !worldToken || !span(engine,0x498) ||
        !array(engine+0x48C,4,players,playerCount) || playerCount!=1) {
        result.reason="single-local-player";return result;
    }
    const auto player=read(players);
    const auto playerToken=token(player);
    result.controller=playerToken && span(player,0x44)?read(player+0x40):0;
    if (!isA(player,"LocalPlayer") || !isA(result.controller,"WillowPlayerController")) {
        result.reason="local-controller";return result;
    }
    std::uint32_t pawnProperty=0,pawnOffset=0;
    const auto controllerToken=token(result.controller);
    auto type=read(result.controller+0x34);
    for (unsigned depth=0;type && depth<48 && !pawnProperty;++depth) {
        if (!token(type) || !span(type,0x4C) || !named(read(type+0x34),"Class")) break;
        const auto total=read(type+0x48)&0x00FFFFFFu;
        if (!total || total>1048576) break;
        auto field=read(type+0x44);
        for (unsigned i=0;field && i<2048;++i) {
            if (!token(field) || !span(field,0x78) || read(field+0x28)!=type) break;
            if (named(field,"Pawn")) {
                const auto offset=read(field+0x54);
                const auto declaredPawn=read(field+0x74);
                if (named(type,"Controller") && named(read(field+0x34),"ObjectProperty") &&
                    read(field+0x40)==1 && read(field+0x44)==4 &&
                    named(declaredPawn,"Pawn") && named(read(declaredPawn+0x34),"Class") &&
                    span(result.controller,total) && offset<=total && 4<=total-offset &&
                    span(result.controller+offset,4)) { pawnProperty=field;pawnOffset=offset; }
                break;
            }
            field=read(field+0x3C);
        }
        type=read(type+0x40);
    }
    if (!pawnProperty) { result.reason="Pawn-property-schema";return result; }
    result.localPawn=read(result.controller+pawnOffset);
    result.localToken=token(result.localPawn);
    result.localKnown=result.localToken && isA(result.localPawn,"WillowPawn");
    if (generation!=Bl2TouchLifetimeGeneration() || token(target)!=result.targetToken ||
        !token(pawnProperty) || read(result.controller+pawnOffset)!=result.localPawn ||
        token(result.localPawn)!=result.localToken || token(engine)!=engineToken ||
        token(result.world)!=worldToken || token(player)!=playerToken ||
        token(result.controller)!=controllerToken || read(globals.engine)!=engine ||
        read(globals.world)!=result.world || read(player+0x40)!=result.controller) {
        result.targetKnown=false;result.localKnown=false;result.reason="changed-lifetime";return result;
    }
    result.reason=result.localKnown?"original-Pawn-property":"local-Pawn-unknown";
    return result;
}

bool InstallNativePerfProbe(DynarmicHost& host) {
    if (!enabled()) return true;
    if (installed.load(std::memory_order_acquire)) return false;
    auto& loader=host.GetElfLoader();
    const auto base=loader.GetBaseAddress();
    if (loader.GetImageSize()!=0x03089000 ||
        host.FindSymbol("GFrameCounter")!=base+0x02F91B88) return false;
    for (const auto& [symbol,rva]:{std::pair{"GEngine",0x03008D20u},std::pair{"GWorld",0x0300D940u},
            std::pair{"_ZN7UObject11GObjObjectsE",0x02FBA268u},std::pair{"_ZN5FName5NamesE",0x02FB00C8u},
            std::pair{"_ZN7UObject12ProcessEventEP9UFunctionPvS2_",0x010E826Cu}})
        if (host.FindSymbol(symbol)!=base+rva) return false;
    constexpr std::array processEvent{0xE92D4FF0u,0xED2D8B06u,0xE1A04001u,0xE3003402u};
    for (std::size_t i=0;i<processEvent.size();++i)
        if (!loader.IsReadOnlyRange(base+0x010E826C+std::uint32_t(i*4),4) ||
            host.MemoryRead32(base+0x010E826C+std::uint32_t(i*4))!=processEvent[i]) return false;
    std::array<std::uint32_t,kCount> forwards{}, observers{}, replays{};
    for (std::size_t i=0;i<kCount;++i) {
        const auto& entry=kEntries[i];const auto site=base+entry.rva;
        if (host.FindSymbol(entry.symbol)!=site || !loader.IsReadOnlyRange(site,8)) return false;
        if (entry.replay==Replay::Forward) {
            forwards[i]=host.GetThunkManager().FindRegisteredSymbol(entry.existingThunk);
            if (!forwards[i] || host.MemoryRead32(site)!=0xE51FF004 || host.MemoryRead32(site+4)!=forwards[i]) return false;
        } else if (host.MemoryRead32(site)!=entry.words[0] || host.MemoryRead32(site+4)!=entry.words[1]) return false;
        if (entry.literalRva && (!loader.IsReadOnlyRange(base+entry.literalRva,4) ||
            host.MemoryRead32(base+entry.literalRva)!=entry.literalWord)) return false;
    }
    for (std::size_t i=0;i<kCount;++i) {
        const auto code=MakeReplay(kEntries[i],base+kEntries[i].rva,forwards[i]);
        if (!code.count || !(replays[i]=host.CreateCodeVeneer(std::span(code.words).first(code.count)))) return false;
        observers[i]=host.GetThunkManager().RegisterThunk(std::string("native_perf_")+kEntries[i].label,
            [i,replay=replays[i]](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                observe(i,jit.Regs()[14]);
                if (i==kDamageEntry) {
                    damagePawn.store(jit.Regs()[0],std::memory_order_relaxed);
                    damageSummary.store(jit.Regs()[1],std::memory_order_relaxed);
                    damageLr.store(jit.Regs()[14],std::memory_order_relaxed);
                    describeDamage(current,jit.Regs()[0],jit.Regs()[1],jit.Regs()[14]);
                }
                if (i==kTickEntry) {
                    if (giveAmmoRequested && !giveAmmoAttempted.load(std::memory_order_relaxed))
                        originalCheatOnce(current,bl2::touch::sharedMailbox().snapshot(),false);
                    boundary(current,thread.tick,"tick");
                    if (spawnPawnRequested && !spawnPawnAttempted.load(std::memory_order_relaxed))
                        originalCheatOnce(current,bl2::touch::sharedMailbox().snapshot(),true);
                }
                jit.Regs()[15]=replay;
            });
        if (!observers[i]) return false;
    }
    imageBase=base;frameCounter=base+0x02F91B88;
    char giveAmmo[PROP_VALUE_MAX]{};
    giveAmmoRequested=__system_property_get("debug.bl2.native_perf_give_ammo",giveAmmo)>0 && !std::strcmp(giveAmmo,"1");
    char spawnPawn[PROP_VALUE_MAX]{};
    spawnPawnRequested=__system_property_get("debug.bl2.native_perf_spawn_pawn",spawnPawn)>0 && !std::strcmp(spawnPawn,"1");
    for (std::size_t i=0;i<kCount;++i) {
        const auto site=base+kEntries[i].rva;
        host.MemoryWrite32(site,0xE51FF004);host.MemoryWrite32(site+4,observers[i]);
        host.InvalidateGuestCode(site,8);
        if (host.MemoryRead32(site)!=0xE51FF004 || host.MemoryRead32(site+4)!=observers[i]) return false;
    }
    installed.store(true,std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO,kTag,
        "NATIVE_PROBE ready nvsi_entries=%zu tick=%08X damage=%08X capture_property=debug.bl2.native_perf_capture overhead=diagnostic-only",
        kNvsiEntries,kEntries[kTickEntry].rva,kEntries[kDamageEntry].rva);
    return true;
}
std::uint64_t NativePerfSwapStart() {
    return installed.load(std::memory_order_acquire)?clockNs(CLOCK_MONOTONIC):0;
}
void NativePerfPresent(DynarmicHost& host,std::uint64_t swapStart,bool succeeded) {
    if (!swapStart || !installed.load(std::memory_order_acquire)) return;
    const auto end=clockNs(CLOCK_MONOTONIC);
    boundary(host,thread.present,"present",end-swapStart,succeeded);
}
#endif

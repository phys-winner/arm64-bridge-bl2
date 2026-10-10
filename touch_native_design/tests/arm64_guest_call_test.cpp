// The included methods are extracted verbatim from the production host/loader.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>
#include "dynarmic/interface/halt_reason.h"
namespace Dynarmic::A32 {
struct Jit {
    std::array<std::uint32_t,16> regs{};
    std::array<std::uint32_t,64> ext{};
    std::uint32_t cpsr=0,fpscr=0;
    auto& Regs(){return regs;} auto& ExtRegs(){return ext;}
    auto Cpsr() const{return cpsr;} auto Fpscr() const{return fpscr;}
    void SetCpsr(std::uint32_t value){cpsr=value;} void SetFpscr(std::uint32_t value){fpscr=value;}
};
}
struct DynarmicHost;
static thread_local DynarmicHost* s_active_host=nullptr;
static thread_local Dynarmic::A32::Jit* s_active_jit=nullptr;
struct DynarmicHost {
    struct ThreadContext {
        std::unique_ptr<Dynarmic::A32::Jit> jit=std::make_unique<Dynarmic::A32::Jit>();
        std::unique_ptr<Dynarmic::A32::Jit> callback_jit=std::make_unique<Dynarmic::A32::Jit>();
        std::unique_ptr<Dynarmic::A32::Jit> sync_jit;
        std::recursive_mutex mutex;
        bool is_running_jit=true,is_running_callback_jit=false,is_running_sync_jit=false;
        std::uint32_t callback_stack_top=0x500000,sync_stack_top=0;
        std::size_t slot=3;
    } threadState;
    static constexpr std::uint32_t kCallbackStackSize=512*1024;
    static constexpr std::size_t kMaxThreadContexts=64;
    std::size_t m_memory_size=0x100000000ULL;
    std::mutex m_contexts_mutex;
    bool known=true,normal=true,attemptNested=true;
    bool noStack=false,noCore=false;
    unsigned executions=0,allocations=0,frees=0,cores=0;
    ThreadContext* GetOrCreateThreadContext(){return known?&threadState:nullptr;}
    std::uint32_t GuestAlloc(std::size_t size,std::uint32_t align){assert(size==kCallbackStackSize && align==16);++allocations;return noStack?0:0x600000;}
    void GuestFree(std::uint32_t p){assert(p==0x600000);++frees;}
    std::unique_ptr<Dynarmic::A32::Jit> CreateJit(std::size_t id){assert(id==kMaxThreadContexts*2+threadState.slot);++cores;return noCore?nullptr:std::make_unique<Dynarmic::A32::Jit>();}
    bool CallGuest(std::uint32_t,const std::vector<std::uint32_t>&,std::uint32_t&);
    std::uint64_t ExecuteFunction(Dynarmic::A32::Jit& jit,std::uint32_t pc,const std::vector<std::uint32_t>& args,
                                  std::uint32_t top,std::uint32_t bytes,bool* returned) {
        assert(pc==0x40012340 && args==std::vector<std::uint32_t>({1,2,3,4,5,6,7}));
        const bool callback=&jit==threadState.callback_jit.get();
        assert(bytes==kCallbackStackSize && (callback?threadState.is_running_callback_jit:threadState.is_running_sync_jit));
        assert(top==(callback?threadState.callback_stack_top:threadState.sync_stack_top));
        ++executions;
        assert(jit.Cpsr()==s_active_jit->Cpsr() && jit.Fpscr()==s_active_jit->Fpscr());
        auto* original=s_active_jit;s_active_jit=&jit;
        if(attemptNested) {
            std::uint32_t nested=0x13579BDF;
            if(callback)assert(CallGuest(pc,args,nested)==normal && nested==(normal?0u:0x13579BDFu) && executions==2);
            else assert(!CallGuest(pc,args,nested) && nested==0x13579BDF && executions==2);
        }
        jit.regs.fill(0xAABBCCDD);jit.ext.fill(0x99887766);
        jit.cpsr=0xF00F0010;jit.fpscr=0x02000001;*returned=normal;
        s_active_jit=original;
        return 0; // A genuine zero return must be distinguishable from failure.
    }
};
#include "arm64_call_guest_production.inc"
struct ElfSegment {std::uint32_t vaddr,memsz,filesz,flags,offset;};
static constexpr unsigned PF_X=1;
struct ElfLoader {
    std::uint32_t m_load_base=0x40000000;
    std::vector<ElfSegment> m_segments={{0x1000,0x2000,0x2000,5,0},{0x4000,0x1000,0x1000,6,0}};
    bool IsCodeRange(std::uint32_t,std::size_t) const;
};
#include "arm64_code_range_production.inc"
static bool finished(Dynarmic::HaltReason hr,std::uint32_t final_pc,bool expected_thread_exit=false,bool guest_exception=false){
    constexpr std::uint32_t EXIT_TRAP_ARM=4,EXIT_TRAP_THUMB=8;
    bool value=false;bool* returned=&value;
#include "arm64_call_completion_production.inc"
    return value;
}
int main() {
    DynarmicHost host;s_active_host=&host;s_active_jit=host.threadState.jit.get();
    auto& callback=*host.threadState.callback_jit;
    for(unsigned i=0;i<16;++i)callback.regs[i]=0x81000000+i;
    for(unsigned i=0;i<64;++i)callback.ext[i]=0x71000000+i;
    callback.cpsr=0xA80F0010;callback.fpscr=0x02400010;
    const auto regs=callback.regs,primaryRegs=host.threadState.jit->regs;
    const auto ext=callback.ext,primaryExt=host.threadState.jit->ext;
    const std::vector<std::uint32_t> args={1,2,3,4,5,6,7};std::uint32_t result=123;
    assert(host.CallGuest(0x40012340,args,result) && result==0 && host.executions==2);
    assert(callback.regs==regs && callback.ext==ext && callback.cpsr==0xA80F0010 && callback.fpscr==0x02400010);
    assert(host.threadState.jit->regs==primaryRegs && host.threadState.jit->ext==primaryExt && !host.threadState.is_running_callback_jit);
    assert((!host.threadState.is_running_sync_jit && host.threadState.sync_jit->regs==std::array<std::uint32_t,16>{}));
    assert((host.threadState.sync_jit->ext==std::array<std::uint32_t,64>{} && host.allocations==1 && host.cores==1));
    host.executions=0;host.normal=false;result=123;
    assert(!host.CallGuest(0x40012340,args,result) && result==123 && host.executions==2);
    assert(callback.regs==regs && callback.ext==ext && callback.cpsr==0xA80F0010 && callback.fpscr==0x02400010);
    host.executions=0;
    for(unsigned test=0;test<7;++test) {
        host.known=test!=0;host.threadState.is_running_jit=test!=1;host.threadState.is_running_callback_jit=test==2;
        s_active_host=test==3?nullptr:&host;s_active_jit=test==4?nullptr:host.threadState.jit.get();
        auto invalid=args;if(test==6)invalid.push_back(8);
        assert(!host.CallGuest(test==5?0:0x40012340,invalid,result) && host.executions==0 && result==123);
    }
    // A registered callback may be active even when the primary core is idle.
    host.known=true;host.threadState.is_running_jit=false;host.threadState.is_running_callback_jit=true;
    host.threadState.is_running_sync_jit=false;s_active_host=&host;s_active_jit=host.threadState.callback_jit.get();
    host.normal=true;host.attemptNested=false;
    assert(host.CallGuest(0x40012340,args,result) && result==0 && host.executions==1 && host.allocations==1);
    assert(callback.regs==regs && callback.ext==ext && host.threadState.is_running_callback_jit);
    // Failed stack/core allocation does not publish a core or execute/defer work.
    for(unsigned test=0;test<3;++test) {
        DynarmicHost fresh;s_active_host=&fresh;s_active_jit=fresh.threadState.callback_jit.get();
        fresh.threadState.is_running_callback_jit=true;fresh.noStack=test==0;fresh.noCore=test==1;
        if(test==2)fresh.m_memory_size=0x600004;
        result=123;assert(!fresh.CallGuest(0x40012340,args,result) && result==123 && fresh.executions==0 && !fresh.threadState.sync_jit);
        assert(fresh.frees==(test==0?0u:1u));
    }
    ElfLoader elf;
    assert(elf.IsCodeRange(0x40001000,0x2000) && elf.IsCodeRange(0x40002FFF,1));
    assert(!elf.IsCodeRange(0x40001000,0) && !elf.IsCodeRange(0x40000000,1));
    assert(!elf.IsCodeRange(0x40002FFF,2) && !elf.IsCodeRange(0x40004000,1));
    assert(!elf.IsCodeRange(0x3FFFFFFF,1) && !elf.IsCodeRange(UINT32_MAX,2));
    assert(!elf.IsCodeRange(0x40001000,SIZE_MAX));
    for(auto pc:{4u,8u,10u})assert(finished(Dynarmic::HaltReason::UserDefined1,pc));
    assert(!finished(Dynarmic::HaltReason::MemoryAbort,4));
    assert(!finished(Dynarmic::HaltReason::UserDefined3,4,false,true));
    assert(!finished(Dynarmic::HaltReason::UserDefined2,4,true));
    assert(!finished(Dynarmic::HaltReason::UserDefined1,0x40012344));
    assert(!finished(Dynarmic::HaltReason::CacheInvalidation,8));
    std::puts("PASS: production primary/callback/synchronous call state, zero/failure/fourth-depth/allocation guards, executable ranges");
}

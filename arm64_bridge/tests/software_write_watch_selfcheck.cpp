// Optional production backend; independent integer-byte model and null/on Jit differential.
#include "write_watch_fixture.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

extern "C" int __wrap___system_property_get(const char*, char* value) { value[0]='\0'; return 0; }
extern "C" int write_watch_call_abi_check(void (*)(void*),void*);
asm(".text\n.p2align 2\n.global write_watch_call_abi_check\n.type write_watch_call_abi_check,%function\n"
    "write_watch_call_abi_check:\n"
    "stp x29,x30,[sp,#-32]!\nstp x24,x25,[sp,#16]\nmov x24,#0x5A4C\n"
    "mov x16,x0\nmov x0,x1\nblr x16\nmov x25,#0x5A4C\ncmp x24,x25\ncset w0,eq\n"
    "ldp x24,x25,[sp,#16]\nldp x29,x30,[sp],#32\nret\n"
    ".size write_watch_call_abi_check,.-write_watch_call_abi_check\n");

namespace {
using U32=std::uint32_t;
using U64=std::uint64_t;
constexpr U64 kA32Size=U64{1}<<32;
constexpr U32 kCode=0x10000,kStop=0x800,kFpcrMask=0x07FF9F00;
static_assert(sizeof(std::atomic<std::uint8_t>)==1 && alignof(std::atomic<std::uint8_t>)==1);
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
unsigned checks=0, case_id=0;
const char* phase="initialization";
const char* fixture_name="none";
[[noreturn]] void Fail(const char* reason) {
    std::fprintf(stderr,"WRITE_WATCH FAIL phase=%s fixture=%s case=%u %s\n",phase,fixture_name,case_id,reason);
    std::exit(1);
}
void Timeout(int) {
    constexpr char s[]="WRITE_WATCH FAIL bounded alarm expired\n";
    const auto ignored=write(STDERR_FILENO,s,sizeof(s)-1); (void)ignored; _exit(124);
}
U64 NativeFpcr() { U64 value; asm volatile("mrs %0,fpcr":"=r"(value)); return value; }
enum class Mode { Direct, Fault, Callback, Invalid };
struct State { std::array<U32,16> regs; std::array<U32,64> ext; U32 cpsr,fpscr; };
struct Store { U32 address; std::vector<std::uint8_t> bytes; };
struct Snapshot { State state; std::vector<std::uint8_t> data; unsigned reads,writes,cas,svc; };

State Initial(U32 address,unsigned flags,bool big,bool thumb) {
    State s{};
    for (unsigned i=0;i<16;++i) s.regs[i]=0x13579BDFu^(i*0x1020304u);
    for (unsigned i=0;i<64;++i) s.ext[i]=0x7F812345u^(i*0x1110101u);
    s.regs[0]=address; s.regs[1]=0x89ABCDEF; s.regs[2]=0x11223344; s.regs[3]=0x55667788;
    s.regs[4]=0x99AABBCC; s.regs[5]=0xDDEEFF00;
    s.ext[0]=0x7FC12345; s.ext[1]=0x80000001; s.ext[2]=0xFF812345; s.ext[3]=1;
    s.cpsr=(flags<<28)|0x080D0010u|(big?0x200u:0u)|(thumb?0x20u:0u);
    s.fpscr=0xA800009Fu|((flags&3)<<22)|((flags&4)<<22)|((flags&8)<<22);
    return s;
}
std::vector<std::uint8_t> Bytes(U64 value,unsigned n,bool big) {
    std::vector<std::uint8_t> bytes(n);
    for (unsigned i=0;i<n;++i) bytes[big?n-1-i:i]=std::uint8_t(value>>(i*8));
    return bytes;
}
std::vector<Store> PlannedStores(const WriteFixture& f,const State& s,bool big,bool force_cas,Mode mode) {
    if ((f.predicated && !(s.cpsr&(1u<<30))) || f.exclusive==2 ||
        (f.exclusive==1 && (force_cas || mode==Mode::Invalid))) return {};
    const auto a=s.regs[0];
    const auto d0=U64(s.ext[0])|(U64(s.ext[1])<<32),d1=U64(s.ext[2])|(U64(s.ext[3])<<32);
    switch(f.kind) {
    case 0: case 1: case 2: return {{a,Bytes(s.regs[1],f.width,big)}};
    case 3: { auto b=Bytes(s.regs[2],4,big),c=Bytes(s.regs[3],4,big); b.insert(b.end(),c.begin(),c.end()); return {{a,b}}; }
    case 4: return {{a,Bytes(s.ext[0],4,big)}};
    case 5: {
        const auto lo=big?s.ext[1]:s.ext[0],hi=big?s.ext[0]:s.ext[1];
        return {{a,Bytes(lo,4,big)},{U32(a+4),Bytes(hi,4,big)}};
    }
    case 6: return {{a,Bytes(d0,8,big)}};
    case 7: return {{a,Bytes(d0,8,big)},{U32(a+8),Bytes(d1,8,big)}};
    case 8: { auto b=Bytes(s.regs[4],4,big),c=Bytes(s.regs[5],4,big); b.insert(b.end(),c.begin(),c.end()); return {{a,b}}; }
    default: Fail("unknown independent fixture kind");
    }
}

class Machine final : public Dynarmic::A32::UserCallbacks {
public:
    Machine(Mode mode,bool watched,unsigned page_bits,bool big):mode(mode),watched(watched),page_bits(page_bits),
        host_page(std::size_t(sysconf(_SC_PAGESIZE))),flag_count(std::size_t{1}<<(32-page_bits)),
        flags(std::make_unique<std::atomic<std::uint8_t>[]>(flag_count)),expected_flags(flag_count),monitor(1) {
        if (host_page<4096 || (host_page&(host_page-1))) Fail("invalid host page size");
        mapping_size=kA32Size+host_page;
        mapping=static_cast<std::uint8_t*>(mmap(nullptr,mapping_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0));
        if (mapping==MAP_FAILED) Fail("bounded virtual fastmem reservation");
        Dynarmic::A32::UserConfig conf;
        conf.callbacks=this; conf.processor_id=0; conf.global_monitor=&monitor;
        conf.arch_version=Dynarmic::A32::ArchVersion::v8; conf.define_unpredictable_behaviour=true;
        conf.always_little_endian=!big; conf.enable_cycle_counting=false; conf.code_cache_size=16<<20;
        if (mode!=Mode::Callback) conf.fastmem_pointer=reinterpret_cast<std::uintptr_t>(mapping);
        conf.write_watch=watched?reinterpret_cast<std::uint8_t*>(flags.get()):nullptr;
        conf.write_watch_page_bits=page_bits;
        jit=std::make_unique<Dynarmic::A32::Jit>(conf);
    }
    ~Machine() { jit.reset(); munmap(mapping,mapping_size); }
    bool Valid(U32 a,std::size_t n) const {
        const U64 end=U64(a)+n;
        return mode!=Mode::Invalid && a>=data_begin && end<=data_begin+data_size &&
               (mode==Mode::Direct || end<=kA32Size);
    }
    std::uint8_t* Data(U32 a) { return mode==Mode::Direct?mapping+a:shadow.data()+(U64(a)-data_begin); }
    std::uint8_t* DataBase() { return mode==Mode::Direct?mapping+data_begin:shadow.data(); }
    void Notify(U32 a,std::size_t n) {
        if (!watched || !n) return;
        const auto end=std::min(kA32Size,U64(a)+n);
        for (std::size_t p=a>>page_bits;p<=((end-1)>>page_bits);++p) flags[p].store(1,std::memory_order_release);
    }
    void CallbackFPCR() {
        if ((U32(NativeFpcr())&kFpcrMask)!=(jit->Fpscr()&kFpcrMask)) Fail("guest FPCR changed before callback");
    }
    template<class T>T Read(U32 a) {
        CallbackFPCR(); ++reads; T value{};
        if (Valid(a,sizeof(T))) std::memcpy(&value,Data(a),sizeof(T));
        return value;
    }
    template<class T>void Put(U32 a,T value) {
        CallbackFPCR(); ++writes;
        if (Valid(a,sizeof(T))) { std::memcpy(Data(a),&value,sizeof(T)); Notify(a,sizeof(T)); }
    }
    template<class T>bool CAS(U32 a,T value,T expected) {
        CallbackFPCR(); ++cas;
        if (force_cas || !Valid(a,sizeof(T))) return false;
        if (a%alignof(T)) Fail("unaligned exclusive fixture");
        const bool success=__atomic_compare_exchange_n(reinterpret_cast<T*>(Data(a)),&expected,value,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
        if (success) Notify(a,sizeof(T));
        return success;
    }
    std::optional<U32> MemoryReadCode(U32 pc) override {
        if (pc<kCode) Fail("unexpected code outside frozen programs");
        const auto index=(pc-kCode)/0x100,offset=(pc-kCode)%0x100;
        if (index>=std::size(kWriteFixtures) || offset>=kWriteFixtures[index].size || pc%4) Fail("code beyond frozen fixture extent");
        const auto& f=kWriteFixtures[index]; U32 word=0;
        for (unsigned i=0;i<4 && offset+i<f.size;++i) word|=U32(f.code[offset+i])<<(8*i);
        return word;
    }
    bool IsReadOnlyMemory(U32) override { return false; }
    std::uint8_t MemoryRead8(U32 a) override { return Read<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(U32 a) override { return Read<std::uint16_t>(a); }
    U32 MemoryRead32(U32 a) override { return Read<U32>(a); }
    U64 MemoryRead64(U32 a) override { return Read<U64>(a); }
    void MemoryWrite8(U32 a,std::uint8_t v) override { Put(a,v); }
    void MemoryWrite16(U32 a,std::uint16_t v) override { Put(a,v); }
    void MemoryWrite32(U32 a,U32 v) override { Put(a,v); }
    void MemoryWrite64(U32 a,U64 v) override { Put(a,v); }
    bool MemoryWriteExclusive8(U32 a,std::uint8_t v,std::uint8_t e) override { return CAS(a,v,e); }
    bool MemoryWriteExclusive16(U32 a,std::uint16_t v,std::uint16_t e) override { return CAS(a,v,e); }
    bool MemoryWriteExclusive32(U32 a,U32 v,U32 e) override { return CAS(a,v,e); }
    bool MemoryWriteExclusive64(U32 a,U64 v,U64 e) override { return CAS(a,v,e); }
    void InterpreterFallback(U32 pc,std::size_t) override { std::fprintf(stderr,"interpreter_pc=%08X\n",pc); Fail("unexpected fallback"); }
    void ExceptionRaised(U32 pc,Dynarmic::A32::Exception e) override { std::fprintf(stderr,"exception_pc=%08X kind=%u\n",pc,unsigned(e)); Fail("unexpected exception"); }
    void CallSVC(U32 value) override {
        CallbackFPCR(); if (value!=0x5A) Fail("wrong SVC terminal");
        ++svc; jit->Regs()[15]=kStop; jit->HaltExecution();
    }
    void AddTicks(U64) override { Fail("production cycle counting enabled"); }
    U64 GetTicksRemaining() override { Fail("production cycle counting enabled"); }
    void SetData(U32 address,const std::vector<Store>& plan,bool same) {
        data_begin=U64(address)&~(U64(host_page)-1);
        data_size=2*host_page;
        if (data_begin+data_size>mapping_size) Fail("fixture data outside reservation");
        if (mode==Mode::Direct) {
            if (mprotect(mapping+data_begin,data_size,PROT_READ|PROT_WRITE)) Fail("fixture direct pages");
        } else shadow.resize(data_size);
        std::memset(DataBase(),0xA6,data_size);
        if (same) for (const auto& store:plan) if (Valid(store.address,store.bytes.size()))
            std::memcpy(Data(store.address),store.bytes.data(),store.bytes.size());
    }
    Snapshot Run(std::size_t index,const State& initial,const std::vector<Store>& plan,bool same,bool force) {
        SetData(initial.regs[0],plan,same);
        auto golden=std::vector<std::uint8_t>(DataBase(),DataBase()+data_size);
        std::set<std::size_t> dirty;
        for (const auto& store:plan) if (Valid(store.address,store.bytes.size())) {
            std::memcpy(golden.data()+U64(store.address)-data_begin,store.bytes.data(),store.bytes.size());
            if (watched) {
                const auto end=std::min(kA32Size,U64(store.address)+store.bytes.size());
                for (std::size_t p=store.address>>page_bits;p<=((end-1)>>page_bits);++p) dirty.insert(p);
            }
        }
        monitor.Clear(); jit->Reset(); jit->Regs()=initial.regs; jit->ExtRegs()=initial.ext;
        jit->Regs()[15]=kCode+U32(index*0x100); jit->SetCpsr(initial.cpsr); jit->SetFpscr(initial.fpscr);
        const auto before_cpsr=jit->Cpsr(),before_fpscr=jit->Fpscr();
        const auto host_fpcr=NativeFpcr();
        reads=writes=cas=svc=0; force_cas=force;
        if (!write_watch_call_abi_check([](void* p){ static_cast<Machine*>(p)->jit->Run(); },this)) Fail("reserved x24 ABI not restored");
        if (svc!=1 || jit->Regs()[15]!=kStop) Fail("unbounded/incorrect SVC terminal");
        if (NativeFpcr()!=host_fpcr) Fail("host FPCR not restored");
        if (jit->Cpsr()!=before_cpsr || jit->Fpscr()!=before_fpscr) Fail("CPSR/FPSCR changed by memory watch");
        const auto& f=kWriteFixtures[index];
        auto expected_ext=initial.ext;
        if (initial.cpsr&(1u<<30)) expected_ext[4]=initial.ext[5];
        if (jit->ExtRegs()!=expected_ext) Fail("predicated VMOV/raw NaN payload changed");
        if (std::memcmp(DataBase(),golden.data(),data_size)) Fail("independent integer-byte memory model differs");
        if (f.exclusive) {
            const auto result_reg=f.kind==8?6:3;
            const U32 expected=(f.exclusive==2 || force || mode==Mode::Invalid)?1:0;
            if (jit->Regs()[result_reg]!=expected) Fail("exclusive success/failure result differs");
        }
        for (auto p:dirty) expected_flags[p]=1;
        // No writer runs here; reading atomic object representations is bounded,
        // then each actual byte is consumed through its atomic acquire exchange.
        if (std::memcmp(flags.get(),expected_flags.data(),flag_count)) {
            for (std::size_t p=0;p<flag_count;++p) if (flags[p].load(std::memory_order_relaxed)!=expected_flags[p]) {
                std::fprintf(stderr,"dirty_page=%zu actual=%u expected=%u shift=%u address=%08X mode=%u\n",p,unsigned(flags[p].load()),unsigned(expected_flags[p]),page_bits,initial.regs[0],unsigned(mode)); break;
            }
            Fail("full dirty-table model differs");
        }
        for (auto p:dirty) { if (flags[p].exchange(0,std::memory_order_acq_rel)!=1) Fail("dirty consume ordering"); expected_flags[p]=0; }
        return {{jit->Regs(),jit->ExtRegs(),jit->Cpsr(),jit->Fpscr()},golden,reads,writes,cas,svc};
    }
    Mode mode; bool watched; unsigned page_bits;
    std::size_t host_page,flag_count,mapping_size;
    std::uint8_t* mapping=nullptr;
    std::unique_ptr<std::atomic<std::uint8_t>[]> flags;
    std::vector<std::uint8_t> expected_flags,shadow;
    Dynarmic::ExclusiveMonitor monitor;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    U64 data_begin=0; std::size_t data_size=0;
    unsigned reads=0,writes=0,cas=0,svc=0; bool force_cas=false;
};
void Compare(const Snapshot& a,const Snapshot& b) {
    if (a.state.regs!=b.state.regs || a.state.ext!=b.state.ext || a.state.cpsr!=b.state.cpsr || a.state.fpscr!=b.state.fpscr ||
        a.data!=b.data || a.reads!=b.reads || a.writes!=b.writes || a.cas!=b.cas || a.svc!=b.svc) Fail("null/on full architectural/callback differential differs");
    ++checks;
}
bool NeedsAlignment(const WriteFixture& f) { return f.exclusive || std::strncmp(f.name,"arm_stl",7)==0; }
void CheckGroup(unsigned shift,Mode mode,bool big,bool comprehensive) {
    Machine baseline(mode,false,shift,big),candidate(mode,true,shift,big);
    const U64 page=U64{1}<<shift;
    const U32 boundary=shift==31?0x80000000u:U32(0x20000+page);
    const auto before=checks;
    for (std::size_t i=0;i<std::size(kWriteFixtures)-1;++i) {
        const auto& f=kWriteFixtures[i]; fixture_name=f.name;
        if (!comprehensive && (f.thumb || f.exclusive || f.predicated)) continue;
        std::vector<U32> addresses={0x20000u,U32(boundary-f.width),U32(boundary-1),U32(boundary-f.width+1)};
        if (mode==Mode::Invalid) addresses={0x40000u,0xFFF00000u,U32(0x100000000ull-f.width),0xFFFFFFFFu};
        if (mode==Mode::Direct && !NeedsAlignment(f)) addresses.push_back(U32(0x100000000ull-f.width+1));
        std::sort(addresses.begin(),addresses.end()); addresses.erase(std::unique(addresses.begin(),addresses.end()),addresses.end());
        for (auto address:addresses) {
            if (NeedsAlignment(f) && address%std::min(f.width,8u)) continue;
            for (unsigned flag=0;flag<(comprehensive?16u:2u);++flag) {
                const unsigned nzcv=comprehensive?flag:(flag?15u:0u);
                const auto initial=Initial(address,nzcv,big,f.thumb);
                for (unsigned forced=0;forced<(f.exclusive==1?2u:1u);++forced) {
                    ++case_id;
                    const auto plan=PlannedStores(f,initial,big,forced,mode);
                    Compare(baseline.Run(i,initial,plan,false,forced),candidate.Run(i,initial,plan,false,forced));
                }
                if (flag==4 || (!comprehensive && flag==1)) {
                    ++case_id;
                    const auto plan=PlannedStores(f,initial,big,false,mode);
                    Compare(baseline.Run(i,initial,plan,true,false),candidate.Run(i,initial,plan,true,false));
                }
            }
        }
    }
    std::printf("WRITE_WATCH group=PASS shift=%u mode=%u big=%u checks=%u\n",shift,unsigned(mode),unsigned(big),checks-before);
}
}
int main() {
    std::signal(SIGALRM,Timeout); alarm(30);
    phase="production14 differential";
    for (bool big:{false,true}) for (auto mode:{Mode::Direct,Mode::Fault,Mode::Callback,Mode::Invalid}) CheckGroup(14,mode,big,true);
    phase="other bounded page shifts";
    for (unsigned shift:{12u,16u,31u}) CheckGroup(shift,Mode::Direct,false,false);
    alarm(0);
    std::printf("WRITE_WATCH overall=PASS checks=%u programs=%zu full_gpr_fpr_cpsr_fpscr=1 native_fault_fallback=1 recompiled_callback=1 invalid_no_mark=1 failed_exclusive_no_mark=1 successful_exclusive_mark=1 same_value_republication=1 page_crossing=1 last4GiB_guard=1 x24_abi=1 production_null_default=1\n",checks,std::size(kWriteFixtures)-1);
}

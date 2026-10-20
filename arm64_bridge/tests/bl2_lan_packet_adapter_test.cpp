#include "bl2_lan_runtime_adapter.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#if defined(BL2_TEST_STATIC_GLUE)
#include "bridge_glue_execution.h"
#endif

struct Jit {
    std::array<std::uint32_t,16> regs{};
    std::array<std::uint32_t,64> ext{};
    std::uint32_t flags=0x600001D3;
    auto& Regs() { return regs; }
    auto& ExtRegs() { return ext; }
    std::uint32_t Cpsr() const { return flags; }
    void SetCpsr(std::uint32_t value) { flags=value; }
};
struct Host;
struct Manager {
    using Handler=std::function<void(Jit&,Host&)>;
    std::map<std::string,std::uint32_t> symbols{{"old_ammo",0x10000}};
    std::map<std::uint32_t,Handler> handlers;
    std::uint32_t next=0x10008;
    std::uint32_t FindRegisteredSymbol(const std::string& name) const {
        const auto it=symbols.find(name); return it==symbols.end()?0:it->second;
    }
    std::uint32_t RegisterThunk(const std::string& name,Handler handler) {
        assert(!FindRegisteredSymbol(name)); const auto p=next;next+=8;
        symbols[name]=p; handlers[p]=std::move(handler); return p;
    }
};
struct Loader {
    std::uint32_t base=0x40000000,size=bl2::lan::kImageSize;
    bool readonly=true,symbols=true;
    auto GetBaseAddress() const { return base; }
    auto GetImageSize() const { return size; }
    bool IsReadOnlyRange(std::uint32_t p,std::size_t n) const {
        return readonly && p>=base && std::uint64_t(p)+n<=std::uint64_t(base)+0x02D1F9B8;
    }
    std::uint32_t GetSymbolAddress(const char* name) const {
        if (!symbols) return 0;
        if (!std::strcmp(name,"_ZN16UOnlineSubsystem16GetBuildUniqueIdEv")) return base+0x01A7C468;
        for (const auto& [symbol,rva]:bl2::lan::kSubsystemTables) if (!std::strcmp(symbol,name)) return base+rva;
        for (const auto& [symbol,rva]:bl2::lan::kSocketTables) if (!std::strcmp(symbol,name)) return base+rva;
        for (const auto& [symbol,rva]:bl2::lan::kSymbols) if (!std::strcmp(symbol,name)) return base+rva;
        return 0;
    }
};
struct Host {
    Loader loader; Manager manager;
    std::map<std::uint32_t,std::uint8_t> memory;
    std::vector<std::uint32_t> freed;
    struct Call { std::uint32_t target; std::vector<std::uint32_t> args; std::vector<std::uint8_t> bytes; };
    std::vector<Call> calls;
    std::uint32_t next=0x200000,allocations=0,hookWrites=0;
    bool allocationFailure=false;
    Host() {
        for (const auto& s:bl2::lan::kDualSites) {
            MemoryWrite32(loader.base+s.rva,s.first);MemoryWrite32(loader.base+s.rva+4,s.second);
        }
        hookWrites=0;
    }
    auto& GetElfLoader() { return loader; }
    auto& GetThunkManager() { return manager; }
    std::size_t GetGuestMemorySize() const { return 0x100000000ULL; }
    std::uint8_t MemoryRead8(std::uint32_t p) { return memory[p]; }
    std::uint16_t MemoryRead16(std::uint32_t p) { return MemoryRead8(p)|(std::uint16_t(MemoryRead8(p+1))<<8); }
    std::uint32_t MemoryRead32(std::uint32_t p) {
        std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(MemoryRead8(p+i))<<(i*8);return value;
    }
    std::uint64_t MemoryRead64(std::uint32_t p) { return MemoryRead32(p)|(std::uint64_t(MemoryRead32(p+4))<<32); }
    void MemoryWrite8(std::uint32_t p,std::uint8_t v) { memory[p]=v; }
    void MemoryWrite16(std::uint32_t p,std::uint16_t v) { MemoryWrite8(p,std::uint8_t(v));MemoryWrite8(p+1,std::uint8_t(v>>8)); }
    void MemoryWrite32(std::uint32_t p,std::uint32_t v) {
        for (const auto& s:bl2::lan::kDualSites) hookWrites+=p==loader.base+s.rva || p==loader.base+s.rva+4;
        for(unsigned i=0;i<4;++i)MemoryWrite8(p+i,std::uint8_t(v>>(i*8)));
    }
    std::uint32_t GuestAlloc(std::size_t n,std::size_t) {
        ++allocations;if(allocationFailure)return 0;const auto p=next;next+=std::uint32_t((n+15)&~15u);return p;
    }
    void GuestFree(std::uint32_t p) { freed.push_back(p); }
    void AddExecutableRange(std::uint32_t,std::size_t) {}
    void InvalidateGuestCode(std::uint32_t,std::size_t) {}
    std::uint32_t RunGuestCallback(std::uint32_t target,const std::vector<std::uint32_t>& args) {
        if(target==loader.base+0x01A7C468) { assert(args==std::vector<std::uint32_t>{0x60000});return 0x5113BC36; }
        Call call{target,args,{}};assert(args.size()==5);
        for(unsigned i=0;i<args[2];++i)call.bytes.push_back(MemoryRead8(args[1]+i));
        calls.push_back(call);MemoryWrite32(args[3],args[2]);return 1;
    }
    bool CallGuest(std::uint32_t target,const std::vector<std::uint32_t>& args,std::uint32_t& result) {
        result=RunGuestCallback(target,args);return true;
    }
};
constexpr std::uint32_t kOwner=0x50000,kSubsystem=0x60000,kSocket=0x70000,kBeacon=0x71000,kStack=0x80000,kPacket=0x90000;
constexpr std::uint64_t kNonce=0x0123456789ABCDEF;
bl2::lan::Packet MakePacket(bool query,bool v7,std::size_t body=5) {
    bl2::lan::Packet p{};p[0]=8;bl2::lan::Put32(p.data()+1,0x241);bl2::lan::Put32(p.data()+5,0x4DD37BFF);
    p[9]='S';p[10]=query?'Q':'R';bl2::lan::Put32(p.data()+11,0x5113BC36);bl2::lan::Put64(p.data()+15,kNonce);
    for(std::size_t i=0;i<body;++i)p[23+i]=std::uint8_t(i*13);
    if(v7){ bl2::lan::Packet out{};assert(bl2::lan::EmitV7({p.data(),query?23:23+body},out,0x4DD37BFF,query));return out; }
    return p;
}
void Setup(Host& h,Jit& jit,bool query) {
    h.MemoryWrite8(kOwner+0x49,query?1:2);h.MemoryWrite32(kOwner+0xDC,0x4DD37BFF);
    h.MemoryWrite32(kOwner+0x4A,std::uint32_t(kNonce));h.MemoryWrite32(kOwner+0x4E,std::uint32_t(kNonce>>32));
    h.MemoryWrite32(kOwner+0x3C,kSubsystem);h.MemoryWrite32(kSubsystem,h.loader.base+0x02E4A5B0);
    h.MemoryWrite32(h.loader.base+0x02E4A6F8,h.loader.base+0x01A7C468);
    jit.regs[0]=kOwner;jit.regs[1]=kPacket;jit.regs[13]=kStack;jit.regs[14]=0x12345678;
}
void PacketChecks() {
    using namespace bl2::lan;
    for(bool query:{false,true}) for(std::size_t body:{1u,5u,489u}) {
        const auto p=MakePacket(query,true,body);Packet out{};const auto n=query?16:16+body;
        const auto length=NormalizeV7({p.data(),n},out,0x4DD37BFF,query,kNonce,0x5113BC36);assert(length==n+7);
        Packet back{};assert(EmitV7({out.data(),length},back,0x4DD37BFF,query)==n);
        assert(std::equal(p.begin(),p.begin()+n,back.begin()));
        for(std::size_t cut=0;cut<16;++cut)assert(!NormalizeV7({p.data(),cut},out,0x4DD37BFF,query,kNonce,0));
    }
    auto p=MakePacket(false,true);Packet out{};
    for(unsigned at:{0u,1u,2u,6u,7u,8u}) {auto bad=p;bad[at]=at==1?2:std::uint8_t(bad[at]^0x80);assert(!NormalizeV7({bad.data(),21},out,0x4DD37BFF,false,kNonce,0));}
    assert(!NormalizeV7({p.data(),16},out,0x4DD37BFF,false,kNonce,0));
    assert(!NormalizeV7({p.data(),513},out,0x4DD37BFF,false,kNonce,0));
    p=MakePacket(true,true);assert(!NormalizeV7({p.data(),17},out,0x4DD37BFF,true,kNonce,0));
    auto maximum=MakePacket(false,true,496);assert(NormalizeV7({maximum.data(),512},out,0x4DD37BFF,false,kNonce,0)==519);
}
void RuntimeChecks() {
    using namespace bl2::lan;
    Host h;assert(InstallBl2LanPacketAdapter(h));assert(h.manager.FindRegisteredSymbol("old_ammo")==0x10000);
    assert(h.manager.next==0x10038 && h.hookWrites==10);
    const auto allocated=h.allocations;assert(InstallBl2LanPacketAdapter(h)&&h.allocations==allocated&&h.hookWrites==10);
    Jit jit;Setup(h,jit,true);auto p=MakePacket(true,true);WritePacket(h,kPacket,p,16);jit.regs[2]=16;
    const auto oldFlags=jit.flags;h.manager.handlers.at(h.manager.FindRegisteredSymbol(kDualSites[0].name))(jit,h);
    assert(s_receiveDepth==1&&jit.regs[1]!=kPacket&&jit.regs[2]==23&&jit.flags==oldFlags);
    const auto cleanup=jit.regs[14];const auto normalized=jit.regs[1];assert(h.MemoryRead8(normalized)==8);
#if defined(BL2_TEST_STATIC_GLUE)
    // The strict AOT owned-veneer executor handles the exact prologue and literal
    // jump; resumption is at an explicitly listed original offline seed.
    for(unsigned step=0;step<3;++step)
        assert(bl2::aot::glue::Execute(h,jit,jit.regs[15],[](std::uint32_t){assert(false);}));
    assert(jit.regs[13]==kStack-32&&jit.regs[4]==kOwner&&jit.regs[15]==h.loader.base+kDualSites[0].rva+8);
    assert(h.MemoryRead32(kStack-4)==cleanup&&jit.flags==oldFlags);
    jit.regs[13]=kStack;
#endif
    // A nested v8 call shadows the v7 response context and restores it afterwards.
    Jit nested;Setup(h,nested,true);p=MakePacket(true,false);WritePacket(h,kPacket,p,23);nested.regs[2]=23;
    h.manager.handlers.at(h.manager.FindRegisteredSymbol(kDualSites[1].name))(nested,h);
    assert(s_receiveDepth==2&&!s_receiveFrames[1].query&&nested.regs[1]==kPacket);
    h.manager.handlers.at(cleanup)(nested,h);assert(s_receiveDepth==1);
    // Existing FSocket method receives its exact fifth argument; v7 reply keeps body.
    Jit send;send.regs[0]=kSocket;send.regs[1]=kPacket;send.regs[2]=28;send.regs[3]=kStack+8;
    send.regs[4]=kOwner;send.regs[12]=h.loader.base+0x3000;send.regs[13]=kStack;
    h.MemoryWrite32(kOwner+0xEC,kBeacon);h.MemoryWrite32(kBeacon+0x14,kSocket);h.MemoryWrite32(kStack,kBeacon);
    h.MemoryWrite32(kSocket,h.loader.base+0x02E46F50);h.MemoryWrite32(h.loader.base+0x02E46F78,send.regs[12]);
    assert(!h.loader.IsReadOnlyRange(h.MemoryRead32(kSocket),0x2C));
    assert(!h.loader.IsReadOnlyRange(h.MemoryRead32(kSubsystem),0x14C));
    assert(KnownTable(h,h.MemoryRead32(kSocket),kSocketTables));
    assert(KnownTable(h,h.MemoryRead32(kSubsystem),kSubsystemTables));
    p=MakePacket(false,false);WritePacket(h,kPacket,p,28);h.MemoryWrite32(kStack+0x40,0xABCDE);
    Send(h,send,3,h.loader.base+kDualSites[3].rva);
    assert(h.calls.size()==1&&h.calls[0].args[4]==kBeacon&&h.calls[0].bytes.size()==21&&h.calls[0].bytes[0]==7);
    assert(std::equal(p.begin()+23,p.begin()+28,h.calls[0].bytes.begin()+16));
    assert(send.regs[1]==0xABCDE&&send.regs[15]==h.loader.base+kDualSites[3].rva+8);
    h.manager.handlers.at(cleanup)(jit,h);assert(!s_receiveDepth&&jit.regs[14]==0x12345678&&h.freed.back()==normalized);
    // v8 query remains byte-identical, keeps the first output and adds only one v7 SQ.
    send.regs[0]=kSocket;send.regs[1]=kPacket;send.regs[2]=23;send.regs[3]=kStack+8;
    send.regs[7]=kOwner;send.regs[12]=h.loader.base+0x3000;send.regs[13]=kStack;
    p=MakePacket(true,false);WritePacket(h,kPacket,p,23);h.calls.clear();Send(h,send,2,h.loader.base+kDualSites[2].rva);
    assert(h.calls.size()==2&&h.calls[0].bytes==std::vector<std::uint8_t>(p.begin(),p.begin()+23));
    assert(h.calls[1].bytes.size()==16&&h.calls[1].bytes[0]==7&&h.MemoryRead32(kStack+8)==23);
    assert(h.calls[1].args[3]!=kStack+8&&h.calls[0].args[4]==h.calls[1].args[4]);
    // Allocation failure rejects v7 before altering args or leaking a frame.
    Setup(h,jit,true);p=MakePacket(true,true);WritePacket(h,kPacket,p,16);jit.regs[2]=16;h.allocationFailure=true;
    Receive(h,jit,0x123000,cleanup,h.loader.base);assert(!s_receiveDepth&&jit.regs[1]==kPacket&&jit.regs[15]==0x12345678);
}
void GuardChecks() {
    using namespace bl2::lan;
    for(const auto& s:kDualSites)for(unsigned offset:{0u,4u}) {
        Host h;h.MemoryWrite32(h.loader.base+s.rva+offset,h.MemoryRead32(h.loader.base+s.rva+offset)^1);h.hookWrites=0;
        assert(!InstallBl2LanPacketAdapter(h)&&!h.allocations&&!h.hookWrites&&h.manager.next==0x10008);
    }
    for(unsigned failure=0;failure<4;++failure) {
        Host h;if(failure==0)h.loader.readonly=false;if(failure==1)h.loader.symbols=false;
        if(failure==2)h.loader.size-=4096;if(failure==3)h.loader.base=UINT32_MAX-4095;
        assert(!InstallBl2LanPacketAdapter(h)&&!h.allocations&&!h.hookWrites);
    }
}
int main() {
    PacketChecks();GuardChecks();RuntimeChecks();
    std::puts("PASS: packet bounds/nonce, byte-exact v8 and payload, nested cleanup, fifth argument, allocation failure, preimage guards/idempotence, six appended thunk IDs");
}

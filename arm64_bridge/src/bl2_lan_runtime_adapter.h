#pragma once
#include "bl2_lan_discovery_compat.h"
#include "bl2_lan_packet_adapter.h"
#include <string>
#include <vector>

namespace bl2::lan {
struct HookSite { std::uint32_t rva, first, second; const char* name; };
constexpr std::array<HookSite,5> kDualSites{{
    {0x01C85AF0,0xE92D47F0,0xE1A04000,"bl2_lan_v7_base_receive"},
    {0x01F90748,0xE92D4FF0,0xE1A04000,"bl2_lan_v7_steam_receive"},
    {0x01C85364,0xE12FFF3C,0xE3500000,"bl2_lan_v7_query_send"},
    {0x01C85D4C,0xE12FFF3C,0xE59D1040,"bl2_lan_v7_base_send"},
    {0x01F90D64,0xE12FFF3C,0xE59D1050,"bl2_lan_v7_steam_send"},
}};
// These immutable-input vtables reside in a PF_W PT_LOAD / PT_GNU_RELRO.
// ElfLoader::IsReadOnlyRange deliberately does not classify that segment as RO.
constexpr std::array<std::pair<const char*,std::uint32_t>,3> kSubsystemTables{{
    {"_ZTV16UOnlineSubsystem",0x02DEF598},
    {"_ZTV26UOnlineSubsystemCommonImpl",0x02E24650},
    {"_ZTV26UOnlineSubsystemSteamworks",0x02E4A5A8},
}};
constexpr std::array<std::pair<const char*,std::uint32_t>,3> kSocketTables{{
    {"_ZTV10FSocketBSD",0x02E46F48},
    {"_ZTV23FSocketSteamworksClient",0x02E494B8},
    {"_ZTV17FSocketSteamworks",0x02E4A938},
}};
template<class Host,std::size_t N> bool KnownTable(Host& h,std::uint32_t table,
        const std::array<std::pair<const char*,std::uint32_t>,N>& tables) {
    const auto base=h.GetElfLoader().GetBaseAddress();
    for (const auto& [symbol,rva]:tables)
        if (table==base+rva+8 && h.GetElfLoader().GetSymbolAddress(symbol)==base+rva) return true;
    return false;
}
struct ReceiveFrame { const void* host{}; std::uint32_t owner{}, buffer{}, lr{}, sp{}; bool query{}; };
inline thread_local std::array<ReceiveFrame,8> s_receiveFrames{};
inline thread_local std::size_t s_receiveDepth{};
template<class Host> bool Range(Host& h, std::uint32_t p, std::size_t n) {
    return p >= 0x10000 && std::uint64_t(p) + n <= h.GetGuestMemorySize() && std::uint64_t(p) + n <= UINT32_MAX;
}
template<class Host> void ReadPacket(Host& h, std::uint32_t p, Packet& out, std::size_t n) {
    for (std::size_t i=0; i<n; ++i) out[i]=h.MemoryRead8(p+std::uint32_t(i));
}
template<class Host> void WritePacket(Host& h, std::uint32_t p, const Packet& in, std::size_t n) {
    for (std::size_t i=0; i<n; ++i) h.MemoryWrite8(p+std::uint32_t(i),in[i]);
}
template<class Host> bool GuestCall(Host& h,std::uint32_t target,const std::vector<std::uint32_t>& args,std::uint32_t& result) {
#if defined(BL2_NATIVE_ARM32)
    result=h.RunGuestCallback(target,args); return true;
#else
    // A synchronous LAN result must never be silently deferred at callback depth.
    return h.CallGuest(target,args,result);
#endif
}
template<class Jit> void Branch(Jit& jit, std::uint32_t target) {
    jit.SetCpsr((jit.Cpsr() & ~0x20u) | ((target & 1u) << 5));
    jit.Regs()[15]=target & ~1u;
}
template<class Host> std::uint32_t Continuation(Host& h, const HookSite& site, std::uint32_t base) {
    const std::uint32_t words[]{site.first,site.second,0xE51FF004,base+site.rva+8};
#if defined(BL2_NATIVE_ARM32)
    return h.CreateCodeVeneer(words);
#else
    const auto p=h.GuestAlloc(sizeof(words),4);
    if (!p) return 0;
    for (std::size_t i=0;i<4;++i) h.MemoryWrite32(p+std::uint32_t(i*4),words[i]);
    h.AddExecutableRange(p,sizeof(words)); h.InvalidateGuestCode(p,sizeof(words)); return p;
#endif
}
template<class Host> void Receive(Host& h, auto& jit, std::uint32_t original, std::uint32_t cleanup, std::uint32_t base) {
    const auto owner=jit.Regs()[0], data=jit.Regs()[1], size=jit.Regs()[2], lr=jit.Regs()[14];
    Branch(jit,lr);
    if (s_receiveDepth==s_receiveFrames.size()) return;
    if (!size || !Range(h,data,size) || size>kLanReceiveLimit || h.MemoryRead8(data)!=7) {
        // Shadow an outer v7 query even during nested v8 handling for this owner.
        s_receiveFrames[s_receiveDepth++]={&h,owner,0,lr,jit.Regs()[13],false};
        jit.Regs()[14]=cleanup; Branch(jit,original); return;
    }
    // Invalid v7 has no original admissible path. Leave v8's CRC checks untouched.
    Branch(jit,lr);
    if (!Range(h,owner,0xF0)) return;
    const auto state=h.MemoryRead8(owner+0x49);
    if (state!=1 && state!=2) return;
    Packet input{}, output{}; ReadPacket(h,data,input,size);
    const auto game=h.MemoryRead32(owner+0xDC); const auto nonce=h.MemoryRead64(owner+0x4A);
    const bool query=state==1;
    if (!IsV7({input.data(),size},game,query,nonce)) return;
    const auto subsystem=h.MemoryRead32(owner+0x3C);
    if (!Range(h,subsystem,0xCC)) return;
    const auto table=h.MemoryRead32(subsystem);
    if (!KnownTable(h,table,kSubsystemTables) || h.MemoryRead32(table+0x148)!=base+0x01A7C468) return;
    std::uint32_t build=0;
    if (!GuestCall(h,base+0x01A7C468,{subsystem},build)) return;
    const auto count=NormalizeV7({input.data(),size},output,game,query,nonce,build);
    const auto buffer=h.GuestAlloc(count,8);
    if (!buffer) return;
    WritePacket(h,buffer,output,count);
    s_receiveFrames[s_receiveDepth++]={&h,owner,buffer,lr,jit.Regs()[13],query};
    jit.Regs()[1]=buffer; jit.Regs()[2]=std::uint32_t(count); jit.Regs()[14]=cleanup;
    Branch(jit,original);
}
template<class Host> void Cleanup(Host& h, auto& jit) {
    if (!s_receiveDepth) { Branch(jit,0); return; }
    const auto frame=s_receiveFrames[s_receiveDepth-1];
    if (frame.host!=&h || frame.sp!=jit.Regs()[13]) { Branch(jit,0); return; }
    --s_receiveDepth; s_receiveFrames[s_receiveDepth]={}; if (frame.buffer) h.GuestFree(frame.buffer);
    jit.Regs()[14]=frame.lr; Branch(jit,frame.lr);
}
template<class Host> void Send(Host& h, auto& jit, unsigned kind, std::uint32_t site) {
    const auto args=jit.Regs(); const auto target=args[12], stack=args[13];
    std::uint32_t result=0;
    if (Range(h,stack,4) && Range(h,args[0],4) &&
        h.GetElfLoader().IsReadOnlyRange(target & ~1u,4)) {
        const auto table=h.MemoryRead32(args[0]);
        if (!KnownTable(h,table,kSocketTables) || h.MemoryRead32(table+0x28)!=target) {
            jit.Regs()[0]=0; Branch(jit,site+8); return;
        }
        const auto destination=h.MemoryRead32(stack);
        const auto owner=args[kind==2 ? 7 : 4];
        bool scoped=Range(h,owner,0xF0);
        const auto beacon=scoped ? h.MemoryRead32(owner+0xEC) : 0;
        scoped=scoped && Range(h,beacon,0x18) && h.MemoryRead32(beacon+0x14)==args[0] && destination==beacon;
        Packet input{}, output{}; std::size_t count=0;
        if (scoped && args[2]<=kLanReceiveLimit && Range(h,args[1],args[2])) {
            ReadPacket(h,args[1],input,args[2]);
            count=EmitV7({input.data(),args[2]},output,h.MemoryRead32(owner+0xDC),kind==2);
        }
        const bool reply=count && kind!=2 && s_receiveDepth &&
            s_receiveFrames[s_receiveDepth-1].host==&h && s_receiveFrames[s_receiveDepth-1].owner==owner &&
            s_receiveFrames[s_receiveDepth-1].query;
        const auto temporary=count ? h.GuestAlloc(count+4,8) : 0;
        if (temporary) WritePacket(h,temporary,output,count);
        // Original query result/output are authoritative; the extra v7 send has its own output word.
        const bool called=GuestCall(h,target,{args[0],reply && temporary ? temporary : args[1],
            reply && temporary ? std::uint32_t(count) : args[2],args[3],destination},result);
        if (kind==2 && temporary)
            { std::uint32_t ignored=0; if (called) GuestCall(h,target,{args[0],temporary,std::uint32_t(count),temporary+std::uint32_t(count),destination},ignored); }
        if (temporary) h.GuestFree(temporary);
    }
    jit.Regs()[0]=result; jit.Regs()[14]=site+4;
    if (kind==2) { // Displaced CMP R0,#0, including ARM no-borrow carry.
        jit.SetCpsr((jit.Cpsr() & 0x0FFFFFFFu) | (result & 0x80000000u) | (result ? 0 : 0x40000000u) | 0x20000000u);
    } else if (Range(h,stack,kind==3 ? 0x44 : 0x54))
        jit.Regs()[1]=h.MemoryRead32(stack+(kind==3 ? 0x40 : 0x50));
    Branch(jit,site+8);
}
} // namespace bl2::lan

// Call after every previously registered startup hook: existing SVC IDs stay fixed.
template<class Host> bool InstallBl2LanPacketAdapter(Host& h) {
    using namespace bl2::lan;
    auto& loader=h.GetElfLoader(); const auto base=loader.GetBaseAddress(); auto& manager=h.GetThunkManager();
    if (loader.GetImageSize()!=kImageSize || base>UINT32_MAX-kImageSize ||
        loader.GetSymbolAddress("_ZN16UOnlineSubsystem16GetBuildUniqueIdEv")!=base+0x01A7C468) return false;
    for (const auto& [symbol,rva]:kSymbols) if (loader.GetSymbolAddress(symbol)!=base+rva) return false;
    for (const auto& [symbol,rva]:kSubsystemTables) if (loader.GetSymbolAddress(symbol)!=base+rva) return false;
    for (const auto& [symbol,rva]:kSocketTables) if (loader.GetSymbolAddress(symbol)!=base+rva) return false;
    bool installed=true;
    for (const auto& s:kDualSites) {
        const auto known=manager.FindRegisteredSymbol(s.name);
        if (!loader.IsReadOnlyRange(base+s.rva,8)) return false;
        const auto a=h.MemoryRead32(base+s.rva),b=h.MemoryRead32(base+s.rva+4);
        if (known && a==0xE51FF004 && b==known) continue;
        installed=false;
        if (known || a!=s.first || b!=s.second) return false;
    }
    if (installed) return true;
    // Reject a partially installed mix before allocating/registering anything.
    for (const auto& s:kDualSites) if (manager.FindRegisteredSymbol(s.name)) return false;
    std::array<std::uint32_t,2> original{};
    for (unsigned i=0;i<2;++i) { original[i]=Continuation(h,kDualSites[i],base); if (!original[i]) return false; }
    const auto cleanup=manager.RegisterThunk("bl2_lan_v7_receive_cleanup",[](auto& jit,Host& current) { Cleanup(current,jit); });
    if (!cleanup) return false;
    std::array<std::uint32_t,5> thunks{};
    for (unsigned i=0;i<5;++i) {
        const auto site=base+kDualSites[i].rva;
        if (i<2) thunks[i]=manager.RegisterThunk(kDualSites[i].name,
            [target=original[i],cleanup,base](auto& jit,Host& current) { Receive(current,jit,target,cleanup,base); });
        else thunks[i]=manager.RegisterThunk(kDualSites[i].name,
            [i,site](auto& jit,Host& current) { Send(current,jit,i,site); });
        if (!thunks[i]) return false;
    }
    for (unsigned i=0;i<5;++i) {
        const auto site=base+kDualSites[i].rva;
        h.MemoryWrite32(site+4,thunks[i]); h.MemoryWrite32(site,0xE51FF004); h.InvalidateGuestCode(site,8);
        if (h.MemoryRead32(site)!=0xE51FF004 || h.MemoryRead32(site+4)!=thunks[i]) return false;
    }
    return true;
}

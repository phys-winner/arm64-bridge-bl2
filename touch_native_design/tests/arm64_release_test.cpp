// Exact production scope/installer/release methods; guest callback decisions
// are explicit substitutes. Actual displaced ARM instructions run separately.
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <vector>
#include <string>
#include <cstdio>
static constexpr std::uint32_t kMovieKey=0x01CC5E7C,kUiTapTest=64;
static std::uint32_t floatWord(float f){std::uint32_t v=0;std::memcpy(&v,&f,4);return v;}
#include "arm64_release_scope.inc"
namespace Dynarmic::A32 {struct Jit {std::array<std::uint32_t,16> r{};auto& Regs(){return r;}};}
struct DynarmicHost {
    static constexpr std::uint32_t base=0x40000000,veneer=0x20000,thunk=0x30000;
    std::map<std::uint32_t,std::uint32_t> words;
    std::function<void(Dynarmic::A32::Jit&,DynarmicHost&)> handler;
    unsigned originalWrites=0,allocations=0,registrations=0,invalidations=0,calls=0,ups=0;
    bool image=true,ro=true,code=true,symbol=true,allocate=true,registered=true,executed=true,filtered=false,discarded=false,handled=false;
    struct Elf {DynarmicHost& h;auto GetImageSize() const{return h.image?0x03089000u:0u;}
        bool IsReadOnlyRange(std::uint32_t,std::size_t) const{return h.ro;}
        bool IsCodeRange(std::uint32_t,std::size_t) const{return h.code;}} elf{*this};
    auto& GetElfLoader(){return elf;} auto& GetThunkManager(){return *this;}
    std::uint32_t FindSymbol(const char* n){return !symbol?0:base+(std::strcmp(n,"KEY_LeftMouseButton")==0?0x03009F68:kMovieKey);}
    std::uint32_t MemoryRead32(std::uint32_t p){return words.at(p);}
    void MemoryWrite32(std::uint32_t p,std::uint32_t v){words[p]=v;if(p==base+0x01CC6348 || p==base+0x01CC634C)++originalWrites;}
    std::uint32_t GuestAlloc(std::size_t n,std::uint32_t align){assert(n==16 && align==4);++allocations;return allocate?veneer:0;}
    void AddExecutableRange(std::uint32_t p,std::uint32_t n){assert(p==veneer && n==16);}
    void InvalidateGuestCode(std::uint32_t p,std::size_t n){assert((p==veneer && n==16)||(p==base+0x01CC6348 && n==8));++invalidations;}
    template<class F> std::uint32_t RegisterThunk(const char*,F f){++registrations;handler=f;return registered?thunk:0;}
    DynarmicHost(){words={{base+0x01CC6348,0xE5923000},{base+0x01CC634C,0xE5933090},{base+0x01CC6350,0xE12FFF33}};}
    bool CallGuest(std::uint32_t pc,const std::vector<std::uint32_t>& a,std::uint32_t& out){
        assert(pc==base+kMovieKey && a==std::vector<std::uint32_t>({3,0,2,5,0,1}));++calls;
        if(!executed)return false;
        out=0; // Real zero return still completes synchronously.
        if(filtered || discarded || handled)return true;
        const auto sp=0x18000;Dynarmic::A32::Jit jit;
        jit.r[0]=jit.r[2]=4;jit.r[1]=sp+0x1C;jit.r[4]=5;jit.r[5]=0;jit.r[6]=1;jit.r[7]=3;jit.r[9]=2;jit.r[13]=sp;
        for(auto [offset,value]:std::map<unsigned,unsigned>{{0x64,5},{0x68,0},{0x6C,1},{0x1C,3},{0x30,0},{0x24,0},{0x28,0}})words[sp+offset]=value;
        handler(jit,*this);
        if(jit.r[15]==veneer){++ups;assert(words.at(sp+0x24)==expectedX && words.at(sp+0x28)==expectedY);}
        return true;
    }
    std::uint32_t expectedX=0,expectedY=0;
};
#include "arm64_release_install.inc"
struct Consumer {
    struct Target {std::uint32_t owner=1,movie=2,gfx=3,interface=4,controllerId=0,capabilities=kUiTapTest;} selected;
    DynarmicHost host;static constexpr auto base=DynarmicHost::base;
    bool inventoryReleaseReady=true,live=true,retained=true,key=true;
    bool current() const{return live;} bool targetValid(const Target&,bool) const{return retained;}
    std::string name(std::uint32_t) const{return key?"LeftMouseButton":"Other";}
    std::uint32_t read(std::uint32_t p) const{return p==base+0x03009F68?5:0;}
    bool call(std::uint32_t p,const std::vector<std::uint32_t>& a){std::uint32_t result=0;return host.CallGuest(p,a,result);}
#include "arm64_release_call.inc"
};
int main(){
    for(unsigned fault=0;fault<11;++fault){
        uiNativeReleaseBase=0;DynarmicHost h;
        if(fault==1)h.image=false;if(fault==2)h.ro=false;if(fault==3)h.code=false;if(fault==4)h.symbol=false;
        if(fault>=5 && fault<=7)h.words[h.base+0x01CC6348+4*(fault-5)]^=1;
        if(fault==8)h.allocate=false;if(fault==9)h.registered=false;
        if(fault==10)uiNativeReleaseBase=h.base+4;
        assert(InstallUiNativeRelease(h,h.base)==(fault==0));
        assert(h.originalWrites==(fault==0?2u:0u));
        if(fault==0){assert(h.words.at(h.veneer)==0xE5923000 && h.words.at(h.veneer+4)==0xE5933090);
            assert(h.words.at(h.veneer+8)==0xE51FF004 && h.words.at(h.veneer+12)==h.base+0x01CC6350);}
    }
    for(unsigned failure=0;failure<8;++failure){
        uiNativeReleaseBase=0;Consumer s;assert(InstallUiNativeRelease(s.host,s.base));
        s.host.expectedX=floatWord(1250);s.host.expectedY=floatWord(760);
        if(failure==1)s.host.executed=false;if(failure==2)s.host.filtered=true;if(failure==3)s.host.discarded=true;
        if(failure==4)s.host.handled=true;if(failure==5)s.live=false;if(failure==6)s.key=false;
        if(failure==7)s.inventoryReleaseReady=false;
        assert(s.inventoryMouseUpFor(s.selected,1250,760)==(failure==0));
        assert(s.host.ups==(failure==0?1u:0u));
        assert(s.host.calls==(failure<=4?1u:0u));
        assert(uiNativeReleaseScope==nullptr);
    }
    uiNativeReleaseBase=0;Consumer cancel;assert(InstallUiNativeRelease(cancel.host,cancel.base));
    cancel.live=false;cancel.host.expectedX=cancel.host.expectedY=floatWord(-1);
    assert(cancel.inventoryMouseUpFor(cancel.selected,-1,-1,true) && cancel.host.calls==1 && cancel.host.ups==1);
    cancel.retained=false;assert(!cancel.inventoryMouseUpFor(cancel.selected,-1,-1,true) && cancel.host.calls==1);
    std::puts("PASS: A64 production release installer, actual checked native Released/one XY-adapted GFxUP, filter/handled/failure/cleanup guards");
}

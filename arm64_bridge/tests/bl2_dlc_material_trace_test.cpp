#include "bl2_dlc_material_trace.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <set>
#include <thread>

struct Reader {
    std::map<std::uint32_t,std::uint32_t> outer;
    std::map<std::uint32_t,std::string> names;
    std::map<std::uint32_t,std::uint32_t> memory;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> readable;
    bool span(std::uint32_t p,std::size_t n) const {
        for(const auto& [low,high]:readable)if(p>=low&&std::uint64_t(p)+n<=high)return true;
        return false;
    }
    bool object(std::uint32_t p) const { return outer.contains(p); }
    std::string name(std::uint32_t at,unsigned limit) const {
        auto found=names.find(at-0x2C);if(found==names.end()||found->second.size()>limit)return {};
        return found->second;
    }
    std::uint32_t u32(std::uint32_t p) const {
        auto o=outer.find(p-0x28);if(o!=outer.end())return o->second;
        auto v=memory.find(p);return v==memory.end()?0:v->second;
    }
};
int main() {
    using namespace bl2::dlc::trace;
    constexpr auto a=Veneer(kHooks[0],0x40000000),b=Veneer(kHooks[1],0x40000000);
    static_assert(a==std::array<std::uint32_t,8>{0xE3550000,0x18BD83F8,0xE51FF004,0x41181350,
        0xE3550000,0x18BD83F8,0xE51FF004,0x50131841});
    static_assert(b[0]==0xE3500000&&b[1]==0xE5943050&&b[3]==0x4120F528&&b[7]==0x28F52041);
    Reader r;r.outer={{0x10000,0},{0x11000,0x10000},{0x12000,0x11000}};
    r.names={{0x10000,"FX_Orchid_RakkHive"},{0x11000,"Materials"},{0x12000,"Mat_Spatter_Projectile"}};
    assert(FullPath(r,0x12000).text=="FX_Orchid_RakkHive.Materials.Mat_Spatter_Projectile");
    assert(FullPath(r,0x12000).valid);
    assert(!FullPath(r,0).valid&&!FullPath(r,0x80000000).valid);
    r.outer[0x10000]=0x12000;assert(FullPath(r,0x12000).text=="<outer-cycle>");r.outer[0x10000]=0;
    for(const std::string bad:{std::string{},std::string(513,'X'),std::string("bad\nname"),std::string("bad\xFF")}) {
        r.names[0x12000]=bad;assert(!FullPath(r,0x12000).valid);
    }
    r.names[0x12000]="Mat_1";
    r.outer.clear();r.names.clear();
    for(unsigned i=0;i<33;++i){const auto p=0x10000+i*0x100;r.outer[p]=i? p-0x100:0;r.names[p]="X";}
    assert(FullPath(r,0x12000).text=="<outer-depth-limit>");
    assert(FullPath(r,0x11F00).valid);
    for(auto& [p,name]:r.names)name=std::string(512,'X');
    assert(FullPath(r,0x10400).text=="<path-limit>");
    r.readable={{0x20000,0x20100}};
    for(unsigned i=0;i<4;++i)r.memory[0x20000+i*4]=0x03020100+i*0x04040404;
    assert(Guid(r,0x20000)=="000102030405060708090a0b0c0d0e0f");
    assert(Guid(r,0x80000000)=="<unreadable>");
    for(unsigned i=0;i<4;++i){r.memory[0x20010+i*12]=0;r.memory[0x20014+i*12]=0;r.memory[0x20018+i*12]=0;}
    assert(StaticArrays(r,0x20000).validMask==15);
    r.memory[0x20014]=0x80000000;assert(StaticArrays(r,0x20000).validMask==14);
    r.memory[0x20020]=1;r.memory[0x20024]=1;assert(StaticArrays(r,0x20000).validMask==12);
    assert(StaticArrays(r,0xFFFFfff0).validMask==0);
    Budget budget;unsigned row=0;assert(!budget.Take(1,row));budget.Start(100);
    std::array<std::vector<unsigned>,8> rows;std::array<std::thread,8> threads;
    for(unsigned i=0;i<8;++i)threads[i]=std::thread([&,i]{unsigned n;while(budget.Take(100,n))rows[i].push_back(n);});
    for(auto& thread:threads)thread.join();std::set<unsigned> unique;
    for(const auto& v:rows)unique.insert(v.begin(),v.end());
    assert(unique.size()==128&&*unique.begin()==1&&*unique.rbegin()==128&&!budget.Take(100,row));
    budget.Start(100);assert(!budget.Take(15101,row)&&budget.Take(15100,row)&&row==1);
    std::puts("PASS: exact LE/BE veneers, bounded valid/invalid object paths and GUID/array reads, concurrent 128-row cap and 15-second deadline");
}

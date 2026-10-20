#include "bl2_voice_fx_trace.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

struct Fake {
    std::array<unsigned char,0x1000> bytes{};bool changeIdentity=false;unsigned headerReads=0;
    bool span(std::uint32_t p,std::size_t n)const {return p>=0x10000&&p<=0x11000&&n<=0x11000-p;}
    static bool copyMemory(const void* value,std::uint32_t p,void* out,std::size_t n) {
        auto& r=*const_cast<Fake*>(static_cast<const Fake*>(value));
        if(!r.span(p,n))return false;
        if(p==0x10000&&n==16&&r.changeIdentity)r.put(0x1000C,1);
        std::memcpy(out,r.bytes.data()+p-0x10000,n);return true;
    }
    void put(std::uint32_t p,std::uint32_t v){assert(span(p,4));std::memcpy(bytes.data()+p-0x10000,&v,4);}
    void byte(std::uint32_t p,unsigned char v){assert(span(p,1));bytes[p-0x10000]=v;}
};

int main() {
    namespace fx=bl2::voice::fxtrace;
    Fake read;read.put(0x1000C,287859413);read.put(0x10018,0x10100);read.byte(0x1002F,8);
    read.put(0x10104,193319514);read.byte(0x10101,0);read.put(0x10200,0x10300);read.byte(0x10204,1);read.put(0x10300,0x12345678);
    const auto before=read.bytes;
    auto s=fx::Capture(read,0x10000,0,0xABCD0000,0x10200);
    assert(s.nodeOk&&s.target&&s.descriptorOk&&s.chunkOk&&s.fxReadable);
    assert(s.nodeId==287859413&&s.declaredFxId==193319514&&s.shared==0&&s.nodeFlags==8);
    assert(s.fx==0x10300&&s.fxVtable==0x12345678&&s.bypass==1&&s.gameObject==0xABCD0000);
    assert(read.bytes==before);
    read.put(0x1000C,223536463);read.put(0x10104,975520185);read.put(0x10200,0);
    s=fx::Capture(read,0x10000,0,0,0x10200);assert(s.target&&s.descriptorOk&&!s.fx&&!s.fxReadable);
    read.put(0x1000C,1);assert(!fx::Capture(read,0x10000,0,0,0x10200).target);
    read.put(0x1000C,287859413);assert(!fx::Capture(read,0x10000,1,0,0x10200).target);
    assert(!fx::Capture(read,0x10000,0,0,0xFFFFFFFD).descriptorOk);
    assert(!fx::Capture(read,0xFFFFFFE0,0,0,0x10200).nodeOk);
    assert(!fx::Capture(read,0,0,0,0x10200).nodeOk);
    read.put(0x10200,0xE0000000);s=fx::Capture(read,0x10000,0,0,0x10200);
    assert(s.descriptorOk&&s.fx==0xE0000000&&!s.fxReadable);
    read.put(0x10018,0xFFFFFFFF);s=fx::Capture(read,0x10000,0,0,0x10200);assert(s.target&&!s.chunkOk&&s.descriptorOk);
    read.changeIdentity=true;assert(!fx::Capture(read,0x10000,0,0,0x10200).target);
    fx::Budget budget;unsigned row=0;std::uint64_t elapsed=0;
    assert(!budget.Open(0)&&!budget.Take(0,row,elapsed));budget.Start(100);
    for(unsigned i=1;i<=32;++i){assert(budget.Take(101,row,elapsed));assert(row==i&&elapsed==1);}
    assert(!budget.Open(101)&&!budget.Take(101,row,elapsed));budget.Start(200);
    assert(!budget.Take(120201,row,elapsed));assert(budget.Take(120200,row,elapsed)&&elapsed==120000);
    budget.Start(300);std::atomic<unsigned> accepted{0};std::vector<std::thread> threads;
    for(unsigned i=0;i<8;++i)threads.emplace_back([&]{for(unsigned j=0;j<100;++j){unsigned r=0;std::uint64_t e=0;if(budget.Take(301,r,e))++accepted;}});
    for(auto& t:threads)t.join();assert(accepted==32&&budget.rows==32);
    std::puts("PASS: exact two-node/slot filter, valid/null/unreadable descriptors, overflow/stable-ID guards, read-only memory, deadline and concurrent32-row cap");
}

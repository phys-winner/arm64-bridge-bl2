#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bl2::dlc::trace {
struct Hook { std::uint32_t rva, first, second; const char* thunk; };
constexpr std::array<Hook,2> kHooks{{
    {0x01181348,0xE3550000,0x18BD83F8,"bl2_qa_material_cache_result"},
    {0x0120F520,0xE3500000,0xE5943050,"bl2_qa_material_findid_result"},
}};
constexpr std::uint32_t kRows=128;
constexpr std::uint64_t kWindowMs=15000;
constexpr std::uint32_t Swap32(std::uint32_t v) {
    return (v>>24)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|(v<<24);
}
constexpr std::array<std::uint32_t,8> Veneer(const Hook& h,std::uint32_t base) {
    return {h.first,h.second,0xE51FF004,base+h.rva+8,
            h.first,h.second,0xE51FF004,Swap32(base+h.rva+8)};
}
struct Budget {
    std::atomic<std::uint64_t> until{0};
    std::atomic<std::uint32_t> rows{0};
    void Start(std::uint64_t now) { rows.store(0);until.store(now+kWindowMs,std::memory_order_release); }
    bool Take(std::uint64_t now,std::uint32_t& row) {
        const auto deadline=until.load(std::memory_order_acquire);
        if (!deadline || now>deadline) return false;
        auto count=rows.load(std::memory_order_relaxed);
        while(count<kRows) if(rows.compare_exchange_weak(count,count+1,std::memory_order_relaxed)) {row=count+1;return true;}
        return false;
    }
};
struct Path { std::string text; bool valid=false; };
template<class Reader> Path FullPath(Reader& read,std::uint32_t owner) {
    std::array<std::uint32_t,32> visited{};
    std::vector<std::string> parts;
    std::size_t bytes=0;
    for (std::size_t depth=0;owner && depth<visited.size();++depth) {
        for(std::size_t i=0;i<depth;++i) if(visited[i]==owner) return {"<outer-cycle>",false};
        visited[depth]=owner;
        if(!read.object(owner)) return {"<invalid-object>",false};
        auto name=read.name(owner+0x2C,512);
        if(name.empty() || name.size()>512) return {"<invalid-name>",false};
        for(unsigned char c:name) if(c<32 || c>126) return {"<invalid-name>",false};
        bytes+=name.size()+(parts.empty()?0:1);
        if(bytes>2048) return {"<path-limit>",false};
        parts.push_back(std::move(name));
        owner=read.u32(owner+0x28);
    }
    if(owner) return {"<outer-depth-limit>",false};
    if(parts.empty()) return {"<null-object>",false};
    std::string result;
    for(auto p=parts.rbegin();p!=parts.rend();++p) {if(!result.empty())result+='.';result+=*p;}
    return {result,true};
}
template<class Reader> std::string Guid(Reader& read,std::uint32_t address) {
    if(!read.span(address,16))return "<unreadable>";
    constexpr char hex[]="0123456789abcdef";std::string out;out.reserve(32);
    for(unsigned i=0;i<4;++i) {
        const auto word=read.u32(address+i*4);
        for(unsigned b=0;b<4;++b) {const auto v=(word>>(b*8))&255;out+=hex[v>>4];out+=hex[v&15];}
    }
    return out;
}
struct Arrays {
    std::array<std::uint32_t,4> count{},capacity{};
    unsigned validMask=0;
};
template<class Reader> Arrays StaticArrays(Reader& read,std::uint32_t key) {
    Arrays out;
    if(!read.span(key,64))return out;
    for(unsigned i=0;i<4;++i) {
        const auto at=key+16+i*12,p=read.u32(at),n=read.u32(at+4),c=read.u32(at+8);
        out.count[i]=n;out.capacity[i]=c;
        if(n<=c && c<=4096 && (!n || read.span(p,1)))out.validMask|=1u<<i;
    }
    return out;
}
} // namespace bl2::dlc::trace

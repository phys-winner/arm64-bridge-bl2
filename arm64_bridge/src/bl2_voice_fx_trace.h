#pragma once
#include "bl2_dlc_material_trace.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bl2::voice::fxtrace {
inline constexpr std::uint32_t kGetFxRva=0x02672C78;
inline constexpr const char* kGetFxSymbol="_ZN16CAkParameterNode5GetFXEjR8AkFXDescP16CAkRegisteredObj";
inline constexpr std::array<std::uint32_t,4> kPrefix{0xE92D47F0,0xE1A04000,0xE5D0002F,0xE1A06001};
// Only the first word is replaced by the existing inline-SVC mechanism. Both
// veneers return via the exact original POP; the second word is never reached.
inline constexpr std::array<bl2::dlc::trace::Hook,2> kHooks{{
    {0x02672D78,0xE8BD87F0,0xE594C020,"bl2_qa_voice_fx_return"},
    {0x02672E50,0xE8BD87F0,0x008C6510,"bl2_qa_voice_fx_empty_return"},
}};
inline constexpr unsigned kRows=32;
inline constexpr std::uint64_t kWindowMs=120000;
inline bool Target(std::uint32_t id) { return id==287859413u || id==223536463u; }
template<class Reader> bool Copy(Reader& read,std::uint32_t p,void* out,std::size_t n) {
    return p>=0x10000 && n<=UINT32_MAX && p<=UINT32_MAX-n && read.span(p,n) && Reader::copyMemory(&read,p,out,n);
}
template<class Reader> bool NodeId(Reader& read,std::uint32_t node,std::uint32_t& id) {
    std::array<unsigned char,16> bytes{};
    if(!Copy(read,node,bytes.data(),bytes.size()))return false;
    std::memcpy(&id,bytes.data()+12,4);return true;
}
struct Snapshot {
    bool nodeOk=false,target=false,descriptorOk=false,chunkOk=false,fxReadable=false;
    std::uint32_t node=0,nodeId=0,slot=0,gameObject=0,descriptor=0,fx=0,fxVtable=0;
    std::uint32_t chunk=0,declaredFxId=0;
    unsigned bypass=0,shared=0,nodeFlags=0;
};
template<class Reader> Snapshot Capture(Reader& read,std::uint32_t node,std::uint32_t slot,
                                      std::uint32_t gameObject,std::uint32_t descriptor) {
    Snapshot out;out.node=node;out.slot=slot;out.gameObject=gameObject;out.descriptor=descriptor;
    std::array<unsigned char,0x30> header{};
    if(!Copy(read,node,header.data(),header.size()))return out;
    out.nodeOk=true;std::memcpy(&out.nodeId,header.data()+12,4);
    out.target=Target(out.nodeId)&&slot==0;if(!out.target)return out;
    out.nodeFlags=header[0x2F];std::memcpy(&out.chunk,header.data()+0x18,4);
    std::array<unsigned char,8> chunk{};
    if(out.chunk && Copy(read,out.chunk,chunk.data(),chunk.size())) {
        out.chunkOk=true;out.shared=chunk[1];std::memcpy(&out.declaredFxId,chunk.data()+4,4);
    }
    std::array<unsigned char,5> desc{};
    if(!Copy(read,descriptor,desc.data(),desc.size()))return out;
    out.descriptorOk=true;std::memcpy(&out.fx,desc.data(),4);out.bypass=desc[4];
    if(out.fx && Copy(read,out.fx,&out.fxVtable,4))out.fxReadable=true;
    std::uint32_t finalId=0;
    if(!NodeId(read,node,finalId)||finalId!=out.nodeId)out.nodeOk=out.target=false;
    return out;
}
struct Budget {
    std::atomic<std::uint64_t> until{0};std::atomic<unsigned> rows{0};
    void Start(std::uint64_t now) {rows.store(0);until.store(now+kWindowMs,std::memory_order_release);}
    bool Open(std::uint64_t now)const {
        const auto end=until.load(std::memory_order_acquire);return end&&now<=end&&rows.load(std::memory_order_relaxed)<kRows;
    }
    bool Take(std::uint64_t now,unsigned& row,std::uint64_t& elapsed) {
        const auto end=until.load(std::memory_order_acquire);if(!end||now>end)return false;
        auto count=rows.load(std::memory_order_relaxed);
        while(count<kRows)if(rows.compare_exchange_weak(count,count+1,std::memory_order_relaxed)) {
            row=count+1;elapsed=now>=end-kWindowMs?now-(end-kWindowMs):0;return true;
        }
        return false;
    }
};
} // namespace bl2::voice::fxtrace

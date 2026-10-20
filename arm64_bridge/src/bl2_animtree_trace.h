#pragma once
#include "bl2_dlc_material_trace.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bl2::animtree::trace {
inline constexpr std::uint32_t kRva=0x01721B74;
inline constexpr const char* kSymbol="_ZN22USkeletalMeshComponent12InitAnimTreeEj";
inline constexpr std::array<std::uint32_t,4> kPrefix{0xE92D4FF0,0xE3510000,0xE59F568C,0xE24DD024};
inline constexpr bl2::dlc::trace::Hook kHook{0x01721EF8,0xE5943278,0xE1A01004,"bl2_qa_animtree_loop"};
inline constexpr std::uint64_t kWindowMs=120000;
inline constexpr std::uint32_t kArrayLimit=4096;

template<class Reader> bool Copy(Reader& read,std::uint32_t p,void* out,std::size_t n) {
    return p>=0x10000 && n<=UINT32_MAX && p<=UINT32_MAX-n &&
           read.span(p,n) && Reader::copyMemory(&read,p,out,n);
}
template<class Reader> bool Fields(Reader& read,std::uint32_t p,std::uint32_t offset,void* out,std::size_t n) {
    return p<=UINT32_MAX-offset && Copy(read,p+offset,out,n);
}

struct Snapshot {
    std::uint32_t component=0,index=0,loopCount=0,cachedTree=0,componentIndex=0;
    std::uint32_t animations=0,data=0,count=0,capacity=0,node=0,nodeVtable=0,nodeDispatch=0;
    std::uint32_t cachedData=0,cachedCount=0,cachedCapacity=0;
    bool componentOk=false,headerReadable=false,headerBounded=false,indexInBounds=false;
    bool entryRead=false,nodeObject=false,vtableRead=false,dispatchRead=false;
    bool headerStable=false,componentStable=false,cachedHeaderReadable=false;
    bool nullNode()const {return componentStable&&headerStable&&entryRead&&!node;}
};

template<class Reader> Snapshot Capture(Reader& read,std::uint32_t component,std::uint32_t index,
                                      std::uint32_t loopCount,std::uint32_t cachedTree) {
    Snapshot out;out.component=component;out.index=index;out.loopCount=loopCount;out.cachedTree=cachedTree;
    if(!read.object(component))return out;
    out.componentOk=true;
    if(!Fields(read,component,0x20,&out.componentIndex,4))return out;
    std::array<std::uint32_t,4> header{},finalHeader{};
    if(!Fields(read,component,0x274,header.data(),sizeof(header)))return out;
    out.headerReadable=true;out.animations=header[0];out.data=header[1];out.count=header[2];out.capacity=header[3];
    out.headerBounded=out.count<=out.capacity&&out.capacity<=kArrayLimit&&
        (!out.count || (out.data<=UINT32_MAX-out.count*4&&read.span(out.data,std::size_t(out.count)*4)));
    out.indexInBounds=out.headerBounded&&index<out.count;
    if(out.indexInBounds)out.entryRead=Fields(read,out.data,index*4,&out.node,4);
    if(out.entryRead&&out.node&&read.object(out.node)) {
        out.nodeObject=true;out.vtableRead=Copy(read,out.node,&out.nodeVtable,4);
        if(out.vtableRead)out.dispatchRead=Fields(read,out.nodeVtable,0x164,&out.nodeDispatch,4);
    }
    if(cachedTree&&read.object(cachedTree)) {
        std::array<std::uint32_t,3> cached{};
        out.cachedHeaderReadable=Fields(read,cachedTree,0x124,cached.data(),sizeof(cached));
        if(out.cachedHeaderReadable){out.cachedData=cached[0];out.cachedCount=cached[1];out.cachedCapacity=cached[2];}
    }
    out.headerStable=Fields(read,component,0x274,finalHeader.data(),sizeof(finalHeader))&&header==finalHeader;
    std::uint32_t finalIndex=0;
    out.componentStable=read.object(component)&&Fields(read,component,0x20,&finalIndex,4)&&finalIndex==out.componentIndex;
    return out;
}

enum class Kind:unsigned {None,Target,Null};
inline Kind Classify(const Snapshot& s) {
    if(s.nullNode())return Kind::Null;
    if(s.componentStable&&s.headerStable&&s.headerBounded&&s.indexInBounds&&
       s.loopCount==46&&s.count==46&&s.index==0)return Kind::Target;
    return Kind::None;
}
struct Budget {
    std::atomic<std::uint64_t> until{0};
    std::array<std::atomic<unsigned>,2> counts{};
    std::atomic<unsigned> rows{0};
    void Start(std::uint64_t now) {
        for(auto& n:counts)n.store(0,std::memory_order_relaxed);
        rows.store(0,std::memory_order_relaxed);
        until.store(now<=UINT64_MAX-kWindowMs?now+kWindowMs:0,std::memory_order_release);
    }
    bool Open(std::uint64_t now)const {
        const auto end=until.load(std::memory_order_acquire);
        return end&&now<=end&&rows.load(std::memory_order_relaxed)<16;
    }
    bool Take(std::uint64_t now,Kind kind,unsigned& row,std::uint64_t& elapsed) {
        const auto end=until.load(std::memory_order_acquire);
        if(!end||now>end||kind==Kind::None)return false;
        const auto index=static_cast<unsigned>(kind)-1;
        if(index>=counts.size())return false;
        constexpr std::array<unsigned,2> limits{8,8};
        auto n=counts[index].load(std::memory_order_relaxed);
        while(n<limits[index])if(counts[index].compare_exchange_weak(n,n+1,std::memory_order_relaxed)) {
            row=rows.fetch_add(1,std::memory_order_relaxed)+1;
            elapsed=now>=end-kWindowMs?now-(end-kWindowMs):0;return true;
        }
        return false;
    }
};
} // namespace bl2::animtree::trace

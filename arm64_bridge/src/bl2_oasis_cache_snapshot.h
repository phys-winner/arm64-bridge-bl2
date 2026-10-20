#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// QA only. Caller owns game-thread execution and a fresh readable-map filter.
// No guest calls, allocation, mutation, hooks, or manufactured lookup result.
namespace bl2::dlc::cacheqa {
constexpr char kPackage[]="Orchid_OasisShaderCache_SF";
constexpr char kFindIdSymbol[]="_ZN18FMaterialShaderMap6FindIdERK19FStaticParameterSet15EShaderPlatform";
constexpr std::uint32_t kFindIdRva=0x01835AC0,kRegistry6Rva=0x0300E12C;
constexpr std::array<std::uint32_t,4> kFindIdPrefix{{0xE59F20A4,0xE0611201,0xE92D40F8,0xE1A05000}};
// 16 GUID bytes in original guest little-endian memory, then four empty
// TArray headers (data/num/cap = 0). Write these 16 words to 8-aligned guest RAM.
constexpr std::array<std::uint32_t,16> kEmptyStaticKey{{
    0x010B735A,0x4F2DB29A,0xDD1693B4,0xD8AEC53F,0,0,0,0,0,0,0,0,0,0,0,0}};
constexpr std::uint64_t kNeedPostLoad=0x0100000000000000ULL,kAsync=0x0000040000000000ULL;

inline std::uint32_t At(std::uint32_t p,std::uint32_t delta) {
    return p && delta<=UINT32_MAX-p?p+delta:0;
}
template<class R> bool Class(R& r,std::uint32_t p,const char* kind,const char* package) {
    if(!r.object(p) || r.className(p)!=kind)return false;
    const auto c=r.u32(p+0x34);
    return r.object(c) && r.className(c)=="Class" && r.objectName(r.u32(c+0x28))==package;
}
template<class R> std::uint64_t Flags(R& r,std::uint32_t p) {
    return std::uint64_t(r.u32(p+8))|(std::uint64_t(r.u32(p+12))<<32);
}
struct RawHeader {
    bool readable=false,bounded=false;
    std::uint32_t data=0,storageNum=0,capacity=0,inlineHash=0,hashData=0,hashSize=0;
};
template<class R> RawHeader Header(R& r,std::uint32_t p) {
    RawHeader h;if(!r.span(p,0x3C))return h;
    h.readable=true;h.data=r.u32(p);h.storageNum=r.u32(p+4);h.capacity=r.u32(p+8);
    h.inlineHash=r.u32(p+0x30);h.hashData=r.u32(p+0x34);h.hashSize=r.u32(p+0x38);
    h.bounded=h.storageNum<=h.capacity && h.capacity<=262144 && h.hashSize<=1048576 &&
        (!h.storageNum || r.span(h.data,1));
    return h; // Storage Num is not a live key/map count. No hash-chain walk.
}
struct Snapshot {
    bool objectsOk=false,packageOk=false,cacheOk=false,referencerOk=false;
    bool managerOk=false,refsOk=false,ownersOk=false,containsCache=false,retained=false,asyncOk=false;
    std::uint32_t objectCount=0,packageMatches=0,cacheMatches=0,referencerMatches=0;
    std::uint32_t package=0,cache=0,referencer=0,manager=0,fullyLoaded=0,loadTimeBits=0,platform=0;
    std::uint64_t packageFlags=0,cacheFlags=0,referencerFlags=0;
    std::uint32_t refCount=0,ownerCount=0,asyncCount=0,asyncMatches=0;
    std::uint32_t task=0,linker=0,percentageBits=0,exportCursor=0,preloadCursor=0,postloadCursor=0;
    RawHeader cacheMaps,registry6;
};
template<class R> bool TaskName(R& r,std::uint32_t task,std::string& out) {
    if(!r.span(task,0x78))return false;
    const auto p=r.u32(task+4),n=r.u32(task+8),cap=r.u32(task+12);
    if(!n || n>cap || cap>512 || !r.span(p,n))return false;
    out.clear();
    for(std::uint32_t i=0;i<n;++i) {
        const auto c=r.host.MemoryRead8(p+i);
        if(i+1==n)return c==0;
        if(c<32 || c>126)return false;
        out+=char(c);
    }
    return false;
}
inline bool SamePackage(const std::string& name) {
    if(name.size()!=sizeof(kPackage)-1)return false;
    for(std::size_t i=0;i<name.size();++i) {
        auto a=static_cast<unsigned char>(name[i]),b=static_cast<unsigned char>(kPackage[i]);
        if(a>='A'&&a<='Z')a+='a'-'A';if(b>='A'&&b<='Z')b+='a'-'A';
        if(a!=b)return false;
    }
    return true;
}
// Reader requirements are the existing runtime Reader: base, span, u32,
// array, object, objectName, className, and host.MemoryRead8. Null object-table
// slots are normal. Unreadable non-null slots invalidate the scan.
template<class R> Snapshot Capture(R& r,std::uint32_t manager) {
    Snapshot s;s.manager=manager;s.registry6=Header(r,At(r.base,kRegistry6Rva));
    std::uint32_t objects=0,n=0;
    s.objectsOk=r.array(At(r.base,0x02FBA268),2000000,objects,n);s.objectCount=n;
    if(s.objectsOk)for(std::uint32_t i=0;i<n;++i) {
        const auto p=r.u32(objects+i*4);if(!p)continue;
        if(!r.object(p)){s.objectsOk=false;break;}
        if(r.u32(p+0x28)==0 && r.objectName(p)==kPackage && Class(r,p,"Package","Core")) {
            s.package=p;++s.packageMatches;
        }
    }
    if(s.objectsOk && s.packageMatches==1) {
        s.packageOk=r.span(s.package,0x5C);
        if(s.packageOk) {
            s.fullyLoaded=r.u32(s.package+0x44);s.loadTimeBits=r.u32(s.package+0x58);s.packageFlags=Flags(r,s.package);
        }
        for(std::uint32_t i=0;i<n;++i) {
            const auto p=r.u32(objects+i*4);if(!p)continue;
            if(!r.object(p)){s.objectsOk=false;break;}
            if(r.u32(p+0x28)!=s.package)continue;
            const auto name=r.objectName(p);
            if(name=="CacheObject" && Class(r,p,"ShaderCache","Engine")){s.cache=p;++s.cacheMatches;}
            if(name=="ObjectReferencer" && Class(r,p,"ObjectReferencer","Engine")){s.referencer=p;++s.referencerMatches;}
        }
        s.cacheOk=s.objectsOk && s.cacheMatches==1 && r.span(s.cache,0xF4);
        s.referencerOk=s.objectsOk && s.referencerMatches==1 && r.span(s.referencer,0x48);
        if(s.cacheOk) {
            s.cacheFlags=Flags(r,s.cache);s.platform=r.u32(s.cache+0x78)&255u;s.cacheMaps=Header(r,s.cache+0xB8);
        }
        if(s.referencerOk) {
            s.referencerFlags=Flags(r,s.referencer);
            std::uint32_t refs=0;s.refsOk=r.array(s.referencer+0x3C,65536,refs,s.refCount);
            if(s.refsOk && s.cacheOk)for(std::uint32_t i=0;i<s.refCount;++i)
                if(r.u32(refs+i*4)==s.cache)s.containsCache=true;
        }
    }
    s.managerOk=Class(r,manager,"WillowPlayerPawnDataManager","WillowGame") && r.span(manager,0x54);
    if(s.managerOk) {
        std::uint32_t owners=0;s.ownersOk=r.array(manager+0x48,65536,owners,s.ownerCount);
        if(s.ownersOk && s.referencerOk)for(std::uint32_t i=0;i<s.ownerCount;++i)
            if(r.u32(owners+i*4)==s.referencer)s.retained=true;
    }
    std::uint32_t tasks=0;s.asyncOk=r.array(At(r.base,0x02FFA400),1024,tasks,s.asyncCount);
    if(s.asyncOk)for(std::uint32_t i=0;i<s.asyncCount;++i) {
        const auto task=r.u32(tasks+i*4);std::string name;
        if(!TaskName(r,task,name)){s.asyncOk=false;break;}
        if(SamePackage(name)) {
            ++s.asyncMatches;s.task=task;s.linker=r.u32(task+0x28);s.percentageBits=r.u32(task+0x70);
            s.exportCursor=r.u32(task+0x3C);s.preloadCursor=r.u32(task+0x40);s.postloadCursor=r.u32(task+0x44);
        }
    }
    return s;
}
} // namespace bl2::dlc::cacheqa

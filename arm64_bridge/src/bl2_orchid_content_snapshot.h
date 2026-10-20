#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// QA only; run on the game thread with the existing fresh-readable-map Reader.
// Reads existing active hash nodes. Never calls the inserting GetContentState.
namespace bl2::dlc::contentqa {
constexpr std::uint32_t kLimit=4096,kEnd=UINT32_MAX;
struct Snapshot {
    bool objectsOk=false,headerOk=false,valid=false,found=false;
    bool fullyLicensed=false,fullyInstalled=false;
    std::uint32_t manager=0,package=0,data=0,storageNum=0,capacity=0;
    std::uint32_t hashData=0,hashSize=0,visited=0,index=kEnd,state=0;
    std::uint32_t packageMask=0,licensed=0,installed=0;
};
// Reader: span(address,bytes), u32(address), object/objectName/className.
template<class R> Snapshot Capture(R& r,std::uint32_t manager,std::uint32_t package) {
    Snapshot s;s.manager=manager;s.package=package;
    if(manager>UINT32_MAX-0x250 || package>UINT32_MAX-0x48 ||
       !r.object(manager) || !r.object(package) || !r.span(manager,0x250) || !r.span(package,0x48) ||
       r.className(manager)!="WillowDownloadableContentManager" ||
       r.className(package)!="DownloadablePackageDefinition" || r.objectName(package)!="PackageDef_Orchid")return s;
    const auto outer=r.u32(package+0x28);
    if(!r.object(outer) || r.className(outer)!="Package" || r.objectName(outer)!="GD_OrchidPackageDef")return s;
    s.packageMask=r.u32(package+0x44);if(s.packageMask!=3)return s;
    s.objectsOk=true;
    const auto map=manager+0x214;
    s.data=r.u32(map);s.storageNum=r.u32(map+4);s.capacity=r.u32(map+8);
    s.hashData=r.u32(map+0x34);s.hashSize=r.u32(map+0x38);
    if(s.storageNum>s.capacity || s.capacity>kLimit || s.hashSize>kLimit ||
       (s.storageNum && (!s.data || s.data>UINT32_MAX-s.storageNum*20 ||
                        !r.span(s.data,std::size_t(s.storageNum)*20))))return s;
    if(!s.hashSize){s.headerOk=true;s.valid=true;return s;}
    if((s.hashSize&(s.hashSize-1)) || (!s.hashData && s.hashSize!=1))return s;
    const auto buckets=s.hashData?s.hashData:map+0x30;
    if(buckets>UINT32_MAX-s.hashSize*4 || !r.span(buckets,std::size_t(s.hashSize)*4))return s;
    s.headerOk=true;
    std::array<std::uint8_t,kLimit> seen{};
    std::uint32_t matches=0,matchIndex=kEnd,matchState=0,licensed=0,installed=0;
    for(std::uint32_t bucket=0;bucket<s.hashSize;++bucket) {
        auto index=r.u32(buckets+bucket*4);
        while(index!=kEnd) {
            if(index>=s.storageNum || seen[index])return s;
            seen[index]=1;++s.visited;
            const auto entry=s.data+index*20;
            if(r.u32(entry+16)!=bucket)return s;
            if(r.u32(entry)==package) {
                if(++matches!=1)return s;
                matchIndex=index;matchState=entry+4;
                licensed=r.u32(entry+4);installed=r.u32(entry+8);
            }
            index=r.u32(entry+12);
        }
    }
    s.valid=true;s.found=matches==1;
    if(s.found) {
        s.index=matchIndex;s.state=matchState;s.licensed=licensed;s.installed=installed;
        s.fullyLicensed=(licensed&s.packageMask)==s.packageMask;
        s.fullyInstalled=(installed&s.packageMask)==s.packageMask;
    }
    return s;
}
} // namespace bl2::dlc::contentqa

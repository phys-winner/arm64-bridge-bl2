// UNAPPLIED PROPOSAL. CPU protocol tests do not validate these C++ sources.
#include "native_vulkan_seed.h"
#include "obb_sha256.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bl2vk::seed {
namespace {
[[noreturn]] void Bad(const char* reason) { throw std::runtime_error(reason); }
void Check(bool good, const char* reason) { if (!good) Bad(reason); }
std::uint32_t U32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1])<<8 | std::uint32_t(p[2])<<16 | std::uint32_t(p[3])<<24;
}
std::uint64_t U64(const std::uint8_t* p) { return U32(p) | std::uint64_t(U32(p+4))<<32; }
void P32(std::uint8_t* p,std::uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=std::uint8_t(v>>(8*i)); }
void P64(std::uint8_t* p,std::uint64_t v) { P32(p,std::uint32_t(v));P32(p+4,std::uint32_t(v>>32)); }
Digest Hash(const std::uint8_t* p,std::size_t n) { ObbSha256 hash;hash.Update(p,n);return hash.Finalize(); }
bool Nonzero(const Digest& d) { return std::any_of(d.begin(),d.end(),[](auto v){return v!=0;}); }
bool Equal(const std::uint8_t* p,const Digest& d) { return std::memcmp(p,d.data(),d.size())==0; }
}
Digest ParseDigest(const char* hex) {
    Check(hex&&std::strlen(hex)==64,"Seed expected digest must be 64 lowercase hexadecimal digits");
    Digest d{};auto nibble=[](char c)->unsigned { if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;Bad("Seed expected digest is not lowercase hexadecimal"); };
    for(std::size_t i=0;i<32;++i)d[i]=std::uint8_t(nibble(hex[2*i])*16+nibble(hex[2*i+1]));
    Check(Nonzero(d),"Seed expected identity is zero");return d;
}
std::array<std::uint8_t,kRecipeBytes> EncodeRecipe(const Recipe& r) {
    std::array<std::uint8_t,kRecipeBytes> b{};std::memcpy(b.data(),r.vertex.data(),32);std::memcpy(b.data()+32,r.fragment.data(),32);
    std::size_t offset=64;auto put=[&](auto v){P32(b.data()+offset,v);offset+=4;};
    for(auto v:r.colors)put(v);put(r.colorCount);put(r.depthFormat);put(r.topology);put(r.restart);put(r.attributeCount);
    for(auto a:r.attributes)for(auto v:a)put(v);for(auto v:r.state)put(v);
    Check(offset==b.size(),"Internal recipe encoding size");return b;
}
Recipe DecodeRecipe(std::span<const std::uint8_t,kRecipeBytes> b) {
    Recipe r{};std::memcpy(r.vertex.data(),b.data(),32);std::memcpy(r.fragment.data(),b.data()+32,32);
    std::size_t offset=64;auto get=[&](){auto v=U32(b.data()+offset);offset+=4;return v;};
    for(auto& v:r.colors)v=get();r.colorCount=get();r.depthFormat=get();r.topology=get();r.restart=get();r.attributeCount=get();
    for(auto& a:r.attributes)for(auto& v:a)v=get();for(auto& v:r.state)v=get();return r;
}
bool CanonicalRecipe(const Recipe& r) {
    if(!Nonzero(r.vertex)||!Nonzero(r.fragment)||r.colorCount>4||r.attributeCount>16||r.topology>5||r.restart>1)return false;
    // Values pinned to the connected Vulkan headers: LINE_STRIP=2,
    // TRIANGLE_STRIP=4, TRIANGLE_FAN=5. Adjacency is never emitted here.
    if(r.restart&&(r.topology!=2&&r.topology!=4&&r.topology!=5))return false;
    for(std::size_t i=0;i<4;++i)if(r.colors[i]>109||(i>=r.colorCount&&r.colors[i]))return false;
    if(r.depthFormat&&r.depthFormat!=124&&r.depthFormat!=125&&r.depthFormat!=126&&r.depthFormat!=127&&r.depthFormat!=129&&r.depthFormat!=130)return false;
    std::uint32_t locations=0;
    for(std::size_t i=0;i<16;++i){auto a=r.attributes[i];if(i>=r.attributeCount){if(a!=std::array<std::uint32_t,4>{})return false;continue;}
        if(a[0]>=16||!a[1]||a[1]>109||a[2]>2048||a[3]>1||(locations&(1u<<a[0])))return false;locations|=1u<<a[0];}
    auto s=r.state;
    for(auto i:{0,1,3,12,22,23,24,25})if(s[i]>1)return false;
    for(auto i:{2,4,5,6,7,8,9,10,11})if(s[i]>7)return false;
    for(auto i:{13,14,15,16})if(s[i]>14)return false;
    return s[17]<=4&&s[18]<=4&&s[19]<=15&&s[20]<=3&&s[21]<=1;
}
std::uint64_t RequiredFeatures(const Recipe& r) { return (r.state[22]?kDepthClamp:0)|(r.state[24]?kDepthBounds:0); }
std::vector<Recipe> Decode(std::span<const std::uint8_t> bytes,const Identity& expected) {
    Check(bytes.size()>=kHeaderBytes&&bytes.size()<=kHeaderBytes+kMaxMandatory*kRecipeBytes,"Seed length outside bound");
    auto p=bytes.data();Check(std::memcmp(p,"BL2VKSE1",8)==0,"Seed magic mismatch");
    Check(U32(p+8)==1&&U32(p+12)==kHeaderBytes&&U32(p+20)==kRecipeBytes&&U32(p+24)==kRecipeRevision&&U32(p+28)==1,"Seed schema/revision/flags mismatch");
    auto count=U32(p+16);Check(count>0&&count<=kMaxMandatory&&bytes.size()==kHeaderBytes+std::size_t(count)*kRecipeBytes,"Seed count/stride/length mismatch");
    const Digest* identities[]={&expected.backend,&expected.pack,&expected.content,&expected.scope,&expected.receipt};
    for(std::size_t i=0;i<5;++i)Check(Nonzero(*identities[i])&&Equal(p+32+32*i,*identities[i]),"Seed identity mismatch");
    Check(Equal(p+192,Hash(p+kHeaderBytes,bytes.size()-kHeaderBytes)),"Seed payload SHA256 mismatch");
    auto features=U64(p+224);Check(!(features&~std::uint64_t(3))&&!(features&~expected.availableFeatures),"Seed unsupported feature mask");
    std::vector<Recipe> records;records.reserve(count);std::uint64_t actualFeatures=0;
    for(std::size_t i=0;i<count;++i){auto record=p+kHeaderBytes+i*kRecipeBytes;
        if(i)Check(std::lexicographical_compare(record-kRecipeBytes,record,record,record+kRecipeBytes),"Seed records duplicate or not canonical sorted order");
        auto r=DecodeRecipe(std::span<const std::uint8_t,kRecipeBytes>(record,kRecipeBytes));Check(CanonicalRecipe(r),"Seed recipe structural state is invalid");actualFeatures|=RequiredFeatures(r);records.push_back(r);}
    Check(features==actualFeatures,"Seed declared feature mask differs from recipes");return records;
}
std::vector<std::uint8_t> EncodePostDraw(std::span<const PostDraw> records,const PostDrawRun& run) {
    Check(records.size()<=kMaxCandidates,"Postdraw candidate count outside bound");
    std::vector<std::uint8_t> bytes(kPostDrawHeaderBytes+records.size()*kPostDrawRecordBytes);
    auto p=bytes.data();std::memcpy(p,"BL2VKPD1",8);P32(p+8,1);P32(p+12,kPostDrawHeaderBytes);P32(p+16,static_cast<std::uint32_t>(records.size()));P32(p+20,kPostDrawRecordBytes);
    std::memcpy(p+56,run.backend.data(),32);std::memcpy(p+88,run.pack.data(),32);P32(p+120,run.pid);P32(p+124,run.overflow?0:1);
    P64(p+128,run.startNs);P64(p+136,run.rejectedDraws);P64(p+144,run.lateMandatoryCreates);P64(p+152,run.runtimeCreates);P64(p+160,run.startupCreates);
    for(std::size_t i=0;i<records.size();++i){auto& r=records[i];auto dst=p+kPostDrawHeaderBytes+i*kPostDrawRecordBytes;auto recipe=EncodeRecipe(r.recipe);std::memcpy(dst,recipe.data(),recipe.size());P64(dst+460,r.firstDraw);P64(dst+468,r.lastDraw);P64(dst+476,r.uses);P64(dst+484,r.firstNs);P64(dst+492,r.lastNs);}
    auto hash=Hash(p+kPostDrawHeaderBytes,bytes.size()-kPostDrawHeaderBytes);std::memcpy(p+24,hash.data(),32);return bytes;
}
} // namespace bl2vk::seed

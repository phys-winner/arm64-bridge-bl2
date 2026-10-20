// UNCOMPILED PROPOSED CHECK. All input identities below are synthetic.
#include "native_vulkan_seed.h"
#include "obb_sha256.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

using namespace bl2vk::seed;
namespace {
Digest Hash(const void* p,std::size_t n){ObbSha256 hash;hash.Update(static_cast<const std::uint8_t*>(p),n);return hash.Finalize();}
void Put32(std::vector<std::uint8_t>& b,std::size_t offset,std::uint32_t value){for(unsigned i=0;i<4;++i)b[offset+i]=std::uint8_t(value>>(8*i));}
void Put64(std::vector<std::uint8_t>& b,std::size_t offset,std::uint64_t value){Put32(b,offset,std::uint32_t(value));Put32(b,offset+4,std::uint32_t(value>>32));}
void Rehash(std::vector<std::uint8_t>& b){auto hash=Hash(b.data()+kHeaderBytes,b.size()-kHeaderBytes);std::memcpy(b.data()+192,hash.data(),hash.size());}
void Reject(const std::function<void()>& call){bool rejected=false;try{call();}catch(const std::exception&){rejected=true;}assert(rejected);}
Recipe Synthetic(unsigned number=1){Recipe r{};r.vertex[0]=std::uint8_t(number);r.vertex[1]=std::uint8_t(number>>8);r.vertex[2]=1;r.fragment[0]=2;r.colors[0]=37;r.colorCount=1;r.depthFormat=129;r.topology=3;r.attributeCount=1;r.attributes[0]={2,103,8,0};return r;}
std::vector<std::uint8_t> Envelope(std::vector<Recipe> records,const Identity& identity){
    std::vector<std::uint8_t> b(kHeaderBytes+records.size()*kRecipeBytes);std::memcpy(b.data(),"BL2VKSE1",8);
    Put32(b,8,1);Put32(b,12,kHeaderBytes);Put32(b,16,static_cast<std::uint32_t>(records.size()));Put32(b,20,kRecipeBytes);Put32(b,24,kRecipeRevision);Put32(b,28,1);
    const Digest* ids[]={&identity.backend,&identity.pack,&identity.content,&identity.scope,&identity.receipt};for(std::size_t i=0;i<5;++i)std::memcpy(b.data()+32+32*i,ids[i]->data(),32);
    std::uint64_t features=0;for(std::size_t i=0;i<records.size();++i){auto encoded=EncodeRecipe(records[i]);std::memcpy(b.data()+kHeaderBytes+i*kRecipeBytes,encoded.data(),encoded.size());features|=RequiredFeatures(records[i]);}
    Put64(b,224,features);Rehash(b);return b;
}
}
int main(){
    Identity identity{};identity.backend[0]=1;identity.pack[0]=2;identity.content[0]=3;identity.scope[0]=4;identity.receipt[0]=5;identity.availableFeatures=3;
    auto valid=Envelope({Synthetic()},identity);auto decoded=Decode(valid,identity);assert(decoded.size()==1&&EncodeRecipe(decoded[0])==EncodeRecipe(Synthetic()));
    // Independent fixed byte locations catch ABI/field-order changes.
    auto encoded=EncodeRecipe(Synthetic());assert(encoded[64]==37&&encoded[80]==1&&encoded[84]==129&&encoded[88]==3&&encoded[96]==1&&encoded[100]==2&&encoded[104]==103&&encoded[108]==8);
    for(auto offset:{8u,12u,16u,20u,24u,28u}){auto bad=valid;Put32(bad,offset,0xffffffffu);Reject([&]{Decode(bad,identity);});}
    for(auto offset:{32u,64u,96u,128u,160u,192u}){auto bad=valid;bad[offset]^=0xff;Reject([&]{Decode(bad,identity);});}
    for(auto length:{std::size_t(0),std::size_t(7),kHeaderBytes-1,kHeaderBytes,valid.size()-1}){auto bad=valid;bad.resize(length);Reject([&]{Decode(bad,identity);});}
    auto trailing=valid;trailing.push_back(0);Reject([&]{Decode(trailing,identity);});
    auto duplicate=Envelope({Synthetic(),Synthetic()},identity);Reject([&]{Decode(duplicate,identity);});
    auto reversed=Envelope({Synthetic(2),Synthetic(1)},identity);Reject([&]{Decode(reversed,identity);});
    auto inactive=Synthetic();inactive.attributes[1][0]=1;auto bad=Envelope({inactive},identity);Reject([&]{Decode(bad,identity);});
    auto sameLocation=Synthetic();sameLocation.attributeCount=2;sameLocation.attributes[1]=sameLocation.attributes[0];bad=Envelope({sameLocation},identity);Reject([&]{Decode(bad,identity);});
    auto feature=Synthetic();feature.state[22]=1;auto needs=Envelope({feature},identity);auto unavailable=identity;unavailable.availableFeatures=0;Reject([&]{Decode(needs,unavailable);});assert(Decode(needs,identity).size()==1);
    Put64(needs,224,4);Reject([&]{Decode(needs,identity);});
    std::vector<Recipe> limit;for(unsigned low=0;low<256;++low)for(unsigned high=0;high<2;++high){auto r=Synthetic();r.vertex[0]=std::uint8_t(low);r.vertex[1]=std::uint8_t(high);limit.push_back(r);}
    assert(Decode(Envelope(limit,identity),identity).size()==kMaxMandatory);limit.push_back(Synthetic(1000));Reject([&]{Decode(Envelope(limit,identity),identity);});
    Reject([&]{ParseDigest("0");});Reject([&]{ParseDigest("0000000000000000000000000000000000000000000000000000000000000000");});
    PostDraw record{Synthetic(),10,12,2,1001,1003};PostDrawRun run{identity.backend,identity.pack,123,false,1000,0,0,0,1};
    auto receipt=EncodePostDraw(std::span<const PostDraw>(&record,1),run);assert(receipt.size()==kPostDrawHeaderBytes+kPostDrawRecordBytes&&std::memcmp(receipt.data(),"BL2VKPD1",8)==0);
    std::puts("PASS bounded synthetic seed C++ protocol (not GPU/stage lookup/LRU)");return 0;
}

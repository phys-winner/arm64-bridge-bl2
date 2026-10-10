#include "bl2_program_locals_fastpath.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "guest_software_write_watch.h"
#include "obb_sha256.h"
#include "thunk_manager.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace {
constexpr char kSymbol[]="_ZN17FOpenGLDynamicRHI16SetProgramLocalsER18FProgramLocalStatejjjPKv";
constexpr uint32_t kRva=0x01E331A0, kSize=224, kStateSize=0xA8, kDataSize=1024;
std::atomic<bool> enabled{false};
template<class T> T Read(const uint8_t* p) {T v;std::memcpy(&v,p,sizeof(v));return v;}
template<class T> void Write(uint8_t* p,T v) {std::memcpy(p,&v,sizeof(v));}
bool Heap(const DynarmicHost& h,uint64_t p,size_t n) {
    // The anonymous allocator/stack arena is permanently readable/writable.
    // File mappings and uncommitted guest addresses always use the original.
    return DynarmicHost::IsHeapRange(p,n) && p+n<=h.GetGuestMemorySize();
}
bool Overlap(uint64_t a,size_t n,uint64_t b,size_t m) {return a<b+m && b<a+n;}
bool Comparable(uint32_t v) {
    const auto a=v&0x7FFFFFFFu;
    return a<=0x7F800000u && (a==0 || a>=0x00800000u);
}
bool FloatEqual(uint32_t a,uint32_t b) {
    return a==b || ((a|b)&0x7FFFFFFFu)==0;
}
bool Native(DynarmicHost& h,const std::array<uint32_t,16>& r,uint32_t cpsr) {
    if(cpsr&0x0700FE20u)return false; // ARM little-endian only; veneer retains other modes.
    const auto object=r[0], state=r[1], slot=r[2], userOffset=r[3], sp=r[13];
    if(slot>=36 || !Heap(h,state,kStateSize) || !Heap(h,uint64_t(sp)-24,32) ||
       Overlap(sp-24,32,state,kStateSize) || Overlap(sp-24,32,object,0x764))return false;
    auto* mem=h.GetGuestMemoryBase(); auto* st=mem+state;
    const int16_t index=Read<int16_t>(st+12+slot*4);
    uint32_t bytes=Read<uint32_t>(mem+sp), source=Read<uint32_t>(mem+sp+4);
    if(index<0 || !bytes)return true;
    if(!Heap(h,object,0x764))return false;
    const auto table=Read<uint32_t>(mem+object+0x760);
    if(!Heap(h,uint64_t(table)+slot*4,4) || Overlap(uint64_t(table)+slot*4,4,sp-24,32))return false;
    const auto shader=Read<uint32_t>(mem+table+slot*4);
    if(!Heap(h,shader,0x24) || Overlap(shader,0x24,sp-24,32))return false;
    const int16_t cap=Read<int16_t>(st+14+slot*4);
    if(cap<0)return false;
    bytes=std::min(bytes,static_cast<uint32_t>(cap));
    if(!bytes){h.MemoryWrite32(sp,0);return true;}
    const uint32_t offset=(static_cast<uint32_t>(index)<<4)-Read<uint32_t>(mem+shader+0x20)+userOffset;
    const bool vector=(bytes&15)==0;
    const uint32_t length=vector?bytes:bytes<=4?4:bytes&~3u;
    const auto data=Read<uint32_t>(st+0x9C);
    const uint64_t dest=uint64_t(data)+offset;
    if(length>32768 || offset>32768 || (source&3) || (dest&3) ||
       !Heap(h,source,length) || !Heap(h,dest,length) ||
       Overlap(dest,length,source,length) || Overlap(state,kStateSize,dest,length) ||
       Overlap(state,kStateSize,source,length) || Overlap(object,0x764,dest,length) ||
       Overlap(sp-24,32,dest,length) || Overlap(sp-24,32,source,length) ||
       Overlap(sp-24,32,state,kStateSize) || Overlap(sp-24,32,object,0x764))return false;
    const auto* src=mem+source; auto* dst=mem+dest;
    // VCEQ.F32 treats signed zero as equal. Exceptional/denormal operands go
    // through the original NEON code, preserving every guest FPSCR flag.
    if(vector)for(uint32_t i=0;i<length;i+=4)
        if(!Comparable(Read<uint32_t>(src+i)) || !Comparable(Read<uint32_t>(dst+i)))return false;
    const uint32_t step=vector?16:4;
    uint32_t first=length,last=0;
    for(uint32_t i=0;i<length;i+=step) {
        bool equal=true;
        for(uint32_t lane=0;lane<step;lane+=4) {
            const auto a=Read<uint32_t>(src+i+lane), b=Read<uint32_t>(dst+i+lane);
            equal &= vector?FloatEqual(a,b):a==b;
        }
        if(!equal){first=std::min(first,i);last=i;}
    }
    // The wrapper stores its clamped stack argument before either bulk tail.
    if(vector || bytes>4)h.MemoryWrite32(sp,bytes);
    if(first==length)return true;
    const uint32_t start=vector?(offset+first)&~15u:bytes<=4?offset:(offset+first)&~3u;
    // Exact shipped vector tail: its dirty maximum is the last changed BLOCK
    // START, without +16. Do not substitute a conventional half-open range.
    const uint32_t end=vector?(offset+last)&~15u:bytes<=4?offset+4:((offset+last)&~3u)+4;
    std::memcpy(dst+first,src+first,length-first);
    if(!vector && bytes<=4)Write<uint32_t>(st,1);
    const auto low=Read<int32_t>(st+0xA0), high=Read<int32_t>(st+0xA4);
    if(!vector && bytes>4) {
        Write<int32_t>(st+0xA4,std::max(high,static_cast<int32_t>(end)));
        Write<int32_t>(st+0xA0,std::min(low,static_cast<int32_t>(start)));
    } else {
        Write<int32_t>(st+0xA0,std::min(low,static_cast<int32_t>(start)));
        Write<int32_t>(st+0xA4,std::max(high,static_cast<int32_t>(end)));
    }
    if(vector || bytes>4)Write<uint32_t>(st,1);
    NotifyGuestMemoryWritten(static_cast<uint32_t>(dest)+first,length-first);
    NotifyGuestMemoryWritten(state,4);
    NotifyGuestMemoryWritten(state+0xA0,8);
    return true;
}
bool Check(DynarmicHost& h,uint32_t function) {
    const uint32_t block=h.GuestAlloc(8192,16);
    if(!block)return false;
    auto* m=h.GetGuestMemoryBase();
    const uint32_t object=block,state=block+2048,table=block+2304,shader=block+2560;
    const uint32_t data=block+3072,source=block+4352,sp=block+5632,wrapper=block+6144;
    // Call the real function and capture the caller-owned size argument that
    // its bulk-tail dispatcher updates. Callee-saved r6 owns the capture cell.
    const std::array<uint32_t,15> code{0xE92D4070,0xE59D4010,0xE59D5014,0xE59D6018,
        0xE24DD008,0xE58D4000,0xE58D5004,0xE59FC014,0xE12FFF3C,0xE59D4000,
        0xE5864000,0xE28DD008,0xE8BD8070,0xE1A00000,function};
    h.AddExecutableRange(wrapper,sizeof(code));
    bool pass=true;uint32_t random=0x31AB26F5u;
    std::array<uint8_t,kStateSize> expectedState{};
    std::array<uint8_t,kDataSize> expectedData{};
    for(uint32_t test=0;test<256 && pass;++test) {
        std::memset(m+block,0,8192);
        std::memcpy(m+wrapper,code.data(),sizeof(code));h.InvalidateGuestCode(wrapper,sizeof(code));
        const uint32_t slot=test%36;
        Write<uint32_t>(m+object+0x760,table);Write<uint32_t>(m+table+slot*4,shader);
        const std::array<uint32_t,16> sizes{0,1,2,3,4,5,7,8,12,15,16,32,48,64,128,256};
        const auto size=sizes[test%sizes.size()];
        Write<int16_t>(m+state+12+slot*4,test%19==0?-1:2);
        Write<int16_t>(m+state+14+slot*4,test%17==0?16:256);
        Write<uint32_t>(m+state+0x9C,data);
        Write<uint32_t>(m+state+0xA0,test&1?0u:0x7FFFFFFFu);
        Write<uint32_t>(m+state+0xA4,test&2?512u:0u);
        for(uint32_t i=0;i<kDataSize;i+=4) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            const uint32_t value=0x3F000000u|(random&0x007FFFFFu);
            Write<uint32_t>(m+data+i,value);Write<uint32_t>(m+source+i,value);
            if(test%4 && i%16==test%4*4)Write<uint32_t>(m+source+i,value^0x100u);
        }
        // Equal ±0 must keep the earlier destination bits; zeros after the
        // first different Float4 are copied, including their source sign.
        if(test%11==0){Write<uint32_t>(m+data+32,0);Write<uint32_t>(m+source+32,0x80000000u);}
        if(test%7==0){Write<uint32_t>(m+data+36,0x7F800000u);Write<uint32_t>(m+source+36,0x7F800000u);}
        Write<uint32_t>(m+sp,size);Write<uint32_t>(m+sp+4,source);
        std::array<uint32_t,16> regs{};regs[0]=object;regs[1]=state;regs[2]=slot;regs[3]=test%5*4;regs[13]=sp;
        std::array<uint8_t,kStateSize> initialState{};
        std::array<uint8_t,kDataSize> initialData{};
        std::memcpy(initialState.data(),m+state,kStateSize);
        std::memcpy(initialData.data(),m+data,kDataSize);
        NotifyGuestMemoryWritten(block,8192);
        h.RunFunction(wrapper,{object,state,slot,regs[3],size,source,sp+8});
        const auto expectedSize=Read<uint32_t>(m+sp+8);
        std::memcpy(expectedState.data(),m+state,kStateSize);
        std::memcpy(expectedData.data(),m+data,kDataSize);
        std::memcpy(m+state,initialState.data(),kStateSize);
        std::memcpy(m+data,initialData.data(),kDataSize);
        pass=Native(h,regs,0x10u) && Read<uint32_t>(m+sp)==expectedSize &&
            std::memcmp(expectedState.data(),m+state,kStateSize)==0 &&
            std::memcmp(expectedData.data(),m+data,kDataSize)==0;
    }
    // Boundary rejection is atomic: no writes before the original fallback.
    std::array<uint8_t,8192> before{};
    std::array<uint32_t,16> r{};r[0]=object;r[1]=state;r[13]=sp;
    Write<int16_t>(m+state+12,2);Write<int16_t>(m+state+14,256);
    Write<uint32_t>(m+sp,16);Write<uint32_t>(m+sp+4,source);
    Write<uint32_t>(m+source,0x7FC00001u);
    std::memcpy(before.data(),m+block,8192);
    pass &= !Native(h,r,0x10u) && std::memcmp(before.data(),m+block,8192)==0;
    Write<uint32_t>(m+source,1); // denormal
    std::memcpy(before.data(),m+block,8192);
    pass &= !Native(h,r,0x10u) && std::memcmp(before.data(),m+block,8192)==0;
    Write<uint32_t>(m+sp+4,data+32); // alias
    std::memcpy(before.data(),m+block,8192);
    pass &= !Native(h,r,0x10u) && std::memcmp(before.data(),m+block,8192)==0;
    h.GuestFree(block);
    return pass;
}
}
void SetBl2ProgramLocalsEnabled(bool value){enabled.store(value,std::memory_order_relaxed);}
bool InstallBl2ProgramLocalsFastpath(DynarmicHost& h,bool startupEnabled) {
    char prop[PROP_VALUE_MAX]{};
    const auto length=__system_property_get("debug.bl2.locals_fastpath",prop);
    if(length)startupEnabled=length==1 && prop[0]=='1';
    if(!startupEnabled)return true;
    auto& loader=h.GetElfLoader();const auto f=h.FindSymbol(kSymbol);
    if(h.IsJitRunning() || f!=loader.GetBaseAddress()+kRva || !loader.IsCodeRange(f,kSize))return false;
    const std::array<uint32_t,3> address{f,loader.GetBaseAddress()+0x01E2AD54,loader.GetBaseAddress()+0x01E2AE5C};
    const std::array<uint32_t,3> size{kSize,0x108,0xFC};
    const char* sha[]{"9E39C1F61750DBD596A9D0630C3EE18CB13C219FE362DDD83F3CCE1BF42502F4",
        "FC2A9F8E6FAE4A49B5B9D2D47642D38E6282AC6787244BE113D77D71C8C169F5",
        "1036347E21C0F96202E7C905799BE13BECA272871DD3E7BDC5102995E498598D"};
    for(size_t i=0;i<address.size();++i) {
        if(!loader.IsCodeRange(address[i],size[i]))return false;
        ObbSha256 hash;hash.Update(h.GetGuestMemoryBase()+address[i],size[i]);
        if(std::strcmp(ObbSha256::Hex(hash.Finalize()).data(),sha[i]))return false;
    }
    const bool passed=Check(h,f);
    if(__system_property_get("debug.bl2.perf_tests",prop)==1 && prop[0]=='1')
        __android_log_print(ANDROID_LOG_INFO,"BL2PerfCheck","program-locals original/native 256 + reject3: %s",passed?"PASS":"FAIL");
    if(!passed)return false;
    const auto fallback=h.GuestAlloc(32,4);if(!fallback)return false;
    for(uint32_t off:{0u,16u}) {
        h.MemoryWrite32(fallback+off,0xE282C002);
        h.MemoryWrite32(fallback+off+4,0xE51FF004);
        h.MemoryWrite32(fallback+off+8,off?__builtin_bswap32(f+4):f+4);
    }
    h.AddExecutableRange(fallback,32);h.InvalidateGuestCode(fallback,32);
    const auto thunk=h.GetThunkManager().RegisterThunk("bl2_program_locals_fastpath",
        [fallback](Dynarmic::A32::Jit& jit,DynarmicHost& host) {
            if(!enabled.load(std::memory_order_relaxed) || !Native(host,jit.Regs(),jit.Cpsr()))
                jit.Regs()[15]=fallback+((jit.Cpsr()&0x200u)?16u:0u);
            else {
                const auto lr=jit.Regs()[14];
                jit.SetCpsr((jit.Cpsr()&~0x20u)|((lr&1u)<<5));
                jit.Regs()[15]=lr&~1u;
            }
        });
    if(!thunk)return false;
    const auto svc=h.MemoryRead32(thunk);
    if((svc&0xFF000000u)!=0xEF000000u || !(svc&0xFFFFFFu))return false;
    enabled.store(true,std::memory_order_relaxed);
    h.MemoryWrite32(f,svc);h.InvalidateGuestCode(f,4);
    const bool patched=Check(h,f);
    if(__system_property_get("debug.bl2.perf_tests",prop)==1 && prop[0]=='1')
        __android_log_print(ANDROID_LOG_INFO,"BL2PerfCheck","program-locals patched ABI: %s",patched?"PASS":"FAIL");
    if(!patched) {
        h.MemoryWrite32(f,0xE282C002);h.InvalidateGuestCode(f,4);
        enabled.store(false,std::memory_order_relaxed);return false;
    }
    return true;
}

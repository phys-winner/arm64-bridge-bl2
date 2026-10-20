#include "native_host.h"
#include "thunk_manager.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <malloc.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" std::uint64_t NativeCallWords(std::uint32_t, const std::uint32_t*, std::size_t);
static DynarmicHost nativeHost;
DynarmicHost& GetNativeHost() { return nativeHost; }
DynarmicHost::DynarmicHost() = default;
DynarmicHost::~DynarmicHost() = default;
bool DynarmicHost::Initialize() {
    if (manager_) return true;
    manager_ = std::make_unique<ThunkManager>(*this);
    return manager_->RegisterAllThunks();
}
bool DynarmicHost::InitializeNative(void* handle) { return loader_.Attach(handle) && Initialize(); }
std::uint32_t DynarmicHost::GuestAlloc(std::size_t bytes, std::size_t alignment) {
    void* pointer = nullptr;
    alignment = std::max(alignment, sizeof(void*));
    if ((alignment & (alignment-1)) || posix_memalign(&pointer, alignment, std::max(bytes,std::size_t{1}))) return 0;
    return reinterpret_cast<std::uintptr_t>(pointer);
}
void DynarmicHost::GuestFree(std::uint32_t address) { free(reinterpret_cast<void*>(address)); }
std::uint32_t DynarmicHost::GuestAllocSize(std::uint32_t address) { return address ? malloc_usable_size(reinterpret_cast<void*>(address)) : 0; }
std::uint32_t DynarmicHost::GuestAllocMapping(std::size_t bytes,std::size_t alignment) { return GuestAlloc(bytes,alignment); }
std::uint32_t DynarmicHost::CreateCodeVeneer(std::span<const std::uint32_t> words) {
    if (words.empty() || words.size_bytes() > 4096) return 0;
    std::lock_guard lock(codeMutex_);
    if (!codePage_ || codeUsed_+words.size_bytes()>4096) {
        void* page=mmap(nullptr,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (page==MAP_FAILED) return 0;
        codePage_=static_cast<std::uint8_t*>(page); codeUsed_=0;
    }
    auto* output=codePage_+codeUsed_;
    memcpy(output,words.data(),words.size_bytes());
    __builtin___clear_cache(reinterpret_cast<char*>(output),reinterpret_cast<char*>(output+words.size_bytes()));
    codeUsed_+=words.size_bytes();
    return reinterpret_cast<std::uintptr_t>(output);
}
template<class T> static T Read(std::uint32_t address) { T value; memcpy(&value,reinterpret_cast<const void*>(address),sizeof(value)); return value; }
std::uint8_t DynarmicHost::MemoryRead8(std::uint32_t a) { return Read<std::uint8_t>(a); }
std::uint16_t DynarmicHost::MemoryRead16(std::uint32_t a) { return Read<std::uint16_t>(a); }
std::uint32_t DynarmicHost::MemoryRead32(std::uint32_t a) { return Read<std::uint32_t>(a); }
std::uint64_t DynarmicHost::MemoryRead64(std::uint32_t a) { return Read<std::uint64_t>(a); }
void DynarmicHost::Write(std::uint32_t a,const void* bytes,std::size_t count) {
    if (loader_.IsCodeRange(a,count)) {
        const auto pageSize=static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        const auto page=a & ~(pageSize-1);
        const auto length=((a-page+count+pageSize-1)/pageSize)*pageSize;
        if (mprotect(reinterpret_cast<void*>(page),length,PROT_READ|PROT_WRITE|PROT_EXEC)) std::abort();
    }
    memcpy(reinterpret_cast<void*>(a),bytes,count);
}
void DynarmicHost::MemoryWrite8(std::uint32_t a,std::uint8_t v) { Write(a,&v,sizeof(v)); }
void DynarmicHost::MemoryWrite16(std::uint32_t a,std::uint16_t v) { Write(a,&v,sizeof(v)); }
void DynarmicHost::MemoryWrite32(std::uint32_t a,std::uint32_t v) { Write(a,&v,sizeof(v)); }
void DynarmicHost::MemoryWrite64(std::uint32_t a,std::uint64_t v) { Write(a,&v,sizeof(v)); }
void DynarmicHost::InvalidateGuestCode(std::uint32_t address,std::size_t bytes) {
    auto* start=reinterpret_cast<char*>(address);
    __builtin___clear_cache(start,start+bytes);
    if (loader_.IsCodeRange(address,bytes)) {
        const auto p=static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        const auto page=address & ~(p-1);
        mprotect(reinterpret_cast<void*>(page),((address-page+bytes+p-1)/p)*p,PROT_READ|PROT_EXEC);
    }
}
std::uint64_t DynarmicHost::RunGuestCallback64(std::uint32_t target,const std::vector<std::uint32_t>& args) {
    if (!target || args.size()>32) std::abort();
    std::array<std::uint32_t,32> words{};
    std::copy(args.begin(),args.end(),words.begin());
    return NativeCallWords(target,words.data(),args.size());
}
std::uint32_t DynarmicHost::RunGuestCallback(std::uint32_t target,const std::vector<std::uint32_t>& args) { return RunGuestCallback64(target,args); }
extern "C" void NativeDispatchFrame(std::uint32_t* frame) {
    Dynarmic::A32::Jit call;
    std::copy_n(frame,16,call.Regs().begin());
    std::copy_n(frame-32,32,call.ExtRegs().begin());
    std::copy_n(frame-64,32,call.ExtRegs().begin()+32);
    call.SetCpsr(frame[17]);
    call.SetFpscr(frame[-66]);
    GetNativeHost().GetThunkManager().DispatchThunk(frame[16],call);
    std::copy(call.Regs().begin(),call.Regs().end(),frame);
    std::copy_n(call.ExtRegs().begin(),32,frame-32);
    std::copy_n(call.ExtRegs().begin()+32,32,frame-64);
    frame[17]=call.Cpsr();
    frame[-66]=call.Fpscr();
}

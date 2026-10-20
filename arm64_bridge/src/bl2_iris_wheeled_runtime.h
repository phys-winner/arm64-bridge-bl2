#pragma once
#include "bl2_iris_wheeled_binding.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"
#include <android/log.h>
#include <cstdlib>
#include <vector>

// ISOLATED integration candidate; not enabled or installed by this file.
// Root verifies pinned A0 and exact metadata bytes. InstallCallbacks runs at
// the existing startup tail after permanent hooks, before guest JNI/render.
// Initialize never appends callbacks while the audio thread can dispatch.
namespace bl2::iris::wheeled {
namespace impl=wheeled_pawn_arm_candidate;
struct HostApi {
    DynarmicHost& host;
    bool Span(std::uint32_t p,std::size_t n)const{
        return p>=0x10000&&p<=host.GetGuestMemorySize()&&n<=host.GetGuestMemorySize()-p;
    }
    bool Read(std::uint32_t p,void* out,std::size_t n){
        if(!Span(p,n))return false;
        auto* target=static_cast<std::uint8_t*>(out);
        for(std::size_t i=0;i<n;++i)target[i]=host.MemoryRead8(p+std::uint32_t(i));
        return true;
    }
    bool Write(std::uint32_t p,const void* value,std::size_t n){
        if(!Span(p,n))return false;
        const auto* source=static_cast<const std::uint8_t*>(value);
        std::size_t i=0;
        for(;i+4<=n;i+=4){std::uint32_t word=0;std::memcpy(&word,source+i,4);host.MemoryWrite32(p+std::uint32_t(i),word);}
        for(;i<n;++i)host.MemoryWrite8(p+std::uint32_t(i),source[i]);
        return true;
    }
    bool Call(std::uint32_t pc,const std::uint32_t* words,std::size_t count,std::uint32_t& result){
        if(pc<0x10000||count>32)return false;
        const std::vector<std::uint32_t> args= count?
            std::vector<std::uint32_t>(words,words+count):std::vector<std::uint32_t>{};
#if defined(BL2_NATIVE_ARM32)
        result=host.RunGuestCallback(pc,args);return true;
#else
        return host.CallGuest(pc,args,result);
#endif
    }
};
struct State {
    DynarmicHost* Host=nullptr;
    impl::BindingState Binding{};
    impl::CallbackAddresses Callbacks{};
    std::uint32_t Scratch=0;
    bool CallbacksInstalled=false,Inside=false;
};
inline State& Current(){static State state;return state;}
[[noreturn]] inline void CallbackFailure(impl::Status status,const char* method){
    __android_log_print(ANDROID_LOG_ERROR,"BL2IrisNativeNode",
        "callback-failed method=%s status=%u",method,unsigned(status));
    std::abort();
}
inline bool InstallCallbacks(DynarmicHost& host){
    auto& state=Current();
    if(state.CallbacksInstalled)return state.Host==&host;
    state.Host=&host;
    constexpr const char* names[]={"bl2_iris_wheeled_construct","bl2_iris_wheeled_parent_changed",
        "bl2_iris_wheeled_relevant","bl2_iris_wheeled_tick","bl2_iris_wheeled_context"};
    for(const auto* name:names)if(host.GetThunkManager().FindRegisteredSymbol(name))return false;
    std::array<std::uint32_t,5> addresses{};
    for(unsigned i=0;i<addresses.size();++i){
        addresses[i]=host.GetThunkManager().RegisterThunk(names[i],[i](Dynarmic::A32::Jit& jit,DynarmicHost& current){
            auto& s=Current();HostApi api{current};const auto node=jit.Regs()[0];
            impl::Status status=impl::Status::ClassUnavailable;std::uint32_t value=0;
            if(i==0)status=impl::Construct(api,s.Binding.Native,node);
            else if(i==1)status=impl::HasParentClassChanged(api,s.Binding.Native,node,value);
            else if(i==2)status=impl::OnBecomeRelevant(api,s.Binding.Native,node);
            else if(i==3)status=impl::Tick(api,s.Binding.Native,node,impl::WordFloat(jit.Regs()[1]));
            else status=impl::Context(api,node,value);
            if(status!=impl::Status::Ok)CallbackFailure(status,i==3?"TickAnim":(i==2?"OnBecomeRelevant":"class-callback"));
            if(i==1||i==4)jit.Regs()[0]=value;
        });
        if(!addresses[i])return false;
    }
    state.Callbacks={addresses[0],addresses[1],addresses[2],addresses[3],addresses[4]};
    state.CallbacksInstalled=true;return true;
}
// Invoke synchronously on the original game thread before Continue/travel,
// only after external ELF/file pin verification. Do not infer Ready from an ACK.
inline impl::BindingResult Initialize(DynarmicHost& host,const char* absoluteMetadataPath,
        bool exactPinsVerified){
    auto& state=Current();
    if(!exactPinsVerified||state.Inside)return impl::BindingResult::ApiFailure;
    state.Inside=true;struct Exit{State& s;~Exit(){s.Inside=false;}}exit{state};
    if(!state.CallbacksInstalled||state.Host!=&host)return impl::BindingResult::ApiFailure;
    if(!state.Scratch){
        state.Scratch=host.GuestAlloc(2048,16);
        if(!state.Scratch)return impl::BindingResult::ApiFailure;
        for(unsigned i=0;i<2048;i+=4)host.MemoryWrite32(state.Scratch+i,0);
    }
    HostApi api{host};
    const auto result=impl::BindAndLoad(api,state.Binding,host.GetElfLoader().GetBaseAddress(),
        state.Scratch,state.Callbacks,absoluteMetadataPath);
#if HORN_ENABLE_LOGGING || BL2_ENABLE_QA_CONSOLE
    __android_log_print(result==impl::BindingResult::Ready?ANDROID_LOG_INFO:ANDROID_LOG_WARN,
        "BL2IrisNativeNode","binding result=%u stage=%u base_extent=%X linked_extent=%X class=%08X package=%08X cdo=%08X vtable=%08X",
        unsigned(result),unsigned(state.Binding.Stage),state.Binding.BaseExtent,state.Binding.LinkedExtent,
        state.Binding.Descriptor,state.Binding.Package,state.Binding.DefaultObject,state.Binding.Native.Vtable);
#endif
    return result;
}
} // namespace bl2::iris::wheeled

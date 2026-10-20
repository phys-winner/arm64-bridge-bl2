#pragma once
#include "bl2_iris_wheeled_runtime.h"
#include "bl2_iris_input_return.h"
#include "bl2_locale.h"
#include "obb_sha256.h"
#include <cerrno>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

// ISOLATED proposed production gate; no canonical source calls this function.
// Root must enforce AllowInput at the input-hook continuation for every ABI.
namespace bl2::iris::activation {
enum class Result { NotApplicable, Waiting, Ready, Failed };
struct State {bool Done=false,Failed=false,Inside=false;std::uint32_t ScriptClass=0,ScriptCdo=0;WaitBudget Wait;};
inline State& Current(){static State state;return state;}
template<std::size_t N> inline bool Pin(const Bl2LocaleEntry* entry,const char* expected){
    if(!entry||entry->apk_fd<0||entry->file_size!=N||entry->data_offset<0||entry->bink)return false;
    struct stat st{};if(::fstat(entry->apk_fd,&st)||st.st_size<0)return false;
    const auto offset=std::uint64_t(entry->data_offset),length=std::uint64_t(st.st_size);
    if(offset>length||N>length-offset||offset>std::uint64_t(std::numeric_limits<off_t>::max())-N)return false;
    std::array<std::uint8_t,N> bytes{};std::size_t done=0;
    while(done<N){const auto got=::pread(entry->apk_fd,bytes.data()+done,N-done,entry->data_offset+off_t(done));
        if(got<0&&errno==EINTR)continue;if(got<=0)return false;done+=std::size_t(got);}
    ObbSha256::Digest digest{};return ObbSha256::OfMapping(bytes.data(),bytes.size(),digest)&&
        std::strcmp(ObbSha256::Hex(digest).data(),expected)==0;
}
// At the earliest original input hook, including Title, before mailbox and
// the original ProcessInput body. Native initialization is checked before callbacks.
inline Result Tick(DynarmicHost& host,bool pinnedA0,std::uint64_t now){
    auto& state=Current();if(state.Done)return Result::Ready;if(state.Failed)return Result::Failed;
    if(state.Inside)return Result::Waiting;
    if(!Bl2IrisContentAdmitted())return Result::NotApplicable;
    state.Inside=true;struct Exit{State& s;~Exit(){s.Inside=false;}}exit{state};
    const auto fail=[&](){state.Failed=true;return Result::Failed;};
    EnsureBl2OrchidMount(host);
    if(!Bl2OrchidOwnsApp(213210))return state.Wait.Within(now)?Result::Waiting:fail();
    constexpr char nativePath[]="DLC/Iris/Lic/Content/Iris_WheeledPawnSupplement_B.xxx";
    constexpr char scriptPath[]="DLC/Iris/Lic/Content/Iris_BikeMoveScriptSupplement_B.xxx";
    auto* nativeEntry=FindBl2LocaleEntry(nativePath);auto* scriptEntry=FindBl2LocaleEntry(scriptPath);

    // Iris admission is the exact74 profile; missing either supplement is an
    // invariant failure, never an older-profile success.
    if(!pinnedA0||!Pin<4911>(nativeEntry,"84A00CB868C0933915F7EBA17CB4A865D099B9BB643D586F83F01AF45D8C26ED")||
       !Pin<11061>(scriptEntry,"C46BBFC7C6DE046BF9E9D096C47FB37CBCDD07595F22D3A7798A0132B9C10665"))return fail();
    wheeled::HostApi preflight{host};const auto image=host.GetElfLoader().GetBaseAddress();
    std::uint32_t initialized=0,parentClass=0,withinClass=0;
    if(!preflight.Call(image+0x01000AE0,nullptr,0,initialized)||
       !preflight.Read(image+0x03006EE4,&parentClass,4)||
       !preflight.Read(image+0x02FFA36C,&withinClass,4))return fail();
    if(initialized!=1||!parentClass||!withinClass)
        return state.Wait.Within(now)?Result::Waiting:fail();
    const auto bound=wheeled::Initialize(host,nativePath,true);
    if(bound==wheeled_pawn_arm_candidate::BindingResult::Waiting)
        return state.Wait.Within(now)?Result::Waiting:fail();
    if(bound!=wheeled_pawn_arm_candidate::BindingResult::Ready)return fail();
    wheeled::HostApi api{host};const auto base=host.GetElfLoader().GetBaseAddress();
    const auto scratch=host.GuestAlloc(512,16);if(!scratch)return fail();
    struct Free{DynarmicHost& host;std::uint32_t p;~Free(){host.GuestFree(p);}}free{host,scratch};
    const auto put=[&](unsigned off,const char* s){for(unsigned i=0;;++i){host.MemoryWrite8(scratch+off+i,std::uint8_t(s[i]));if(!s[i])break;}};
    put(0,scriptPath);put(160,"WillowGame.Action_BikeMove");put(224,"WillowGame.Default__Action_BikeMove");put(288,"WillowGame.WillowActionSequencePawn");
    const std::uint32_t load[]={0,scratch,0};std::uint32_t package=0,parent=0;
    if(!api.Call(base+0x0100D228,load,3,package)||!package)return fail();
    const auto find=[&](unsigned off,std::uint32_t& out){const std::uint32_t args[]={0,UINT32_MAX,scratch+off,0};return api.Call(base+0x01007DAC,args,4,out)&&out;};
    if(!find(160,state.ScriptClass)||!find(224,state.ScriptCdo)||!find(288,parent))return fail();
    std::uint32_t cdo=0,cls=0,super=0;
    if(!api.Read(state.ScriptClass+0xF0,&cdo,4)||cdo!=state.ScriptCdo||
       !api.Read(state.ScriptCdo+0x34,&cls,4)||cls!=state.ScriptClass||
       !api.Read(state.ScriptClass+0x40,&super,4)||super!=parent)return fail();
    // Exact original AddToRoot owns these two new process-lifetime classes.
    constexpr std::array<std::uint32_t,5> prefix{0xEDD00B02,0xEDDF1B02,0xF26001B1,0xEDC00B02,0xE12FFF1E};
    for(unsigned i=0;i<prefix.size();++i)if(host.MemoryRead32(base+0x01000B1C+i*4)!=prefix[i])return fail();
    if(host.MemoryRead32(base+0x01000B30)!=0x4000||host.MemoryRead32(base+0x01000B34)!=0)return fail();
    for(const auto object:{wheeled::Current().Binding.Descriptor,state.ScriptClass}){
        std::uint32_t ignored=0,flags=0;if(!api.Call(base+0x01000B1C,&object,1,ignored)||
            !api.Read(object+8,&flags,4)||(flags&0x4000)==0)return fail();
    }
    state.Done=true;return Result::Ready;
}
inline bool AllowInput(Result r){return r==Result::NotApplicable||r==Result::Ready;}
} // namespace bl2::iris::activation

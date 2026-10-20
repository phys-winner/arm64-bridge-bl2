#pragma once
#include "bl2_iris_wheeled_arm.h"

// Concrete original-engine registration/load sequence, isolated from the app.
// The engine-thread caller supplies five real guest callback addresses and2048
// bytes of process-lifetime scratch. It must prevent Continue/map load until
// Ready when this experimental class profile is enabled. No UI/order bypass.
namespace wheeled_pawn_arm_candidate {
enum class BindingStage { Waiting, Constructed, Registered, Ready, Failed };
enum class BindingResult {
    Waiting, Ready, ApiFailure, WrongBaseLayout, ExistingClass,
    WrongConstructorState, RegistrationFailed, PackageLoadFailed,
    WrongLinkedProperties, WrongDefaultObject
};
struct BindingState {
    BindingStage Stage = BindingStage::Waiting;
    BindingResult Last = BindingResult::Waiting;
    Runtime Native{};
    Address Descriptor = 0, Package = 0, DefaultObject = 0;
    std::uint32_t BaseExtent = 0, LinkedExtent = 0;
};
template<class Api> bool BindingCall(Api& api, Address pc,
        const std::uint32_t* words, std::size_t count, std::uint32_t& result) {
    return pc >= 4096 && count <= 32 && api.Call(pc,words,count,result);
}
template<class Api> bool BindingString(Api& api, Address target,
        const char* value, std::size_t capacity) {
    if(!value || !capacity)return false;
    std::size_t n=0;while(n<capacity&&value[n])++n;
    return n<capacity&&api.Write(target,value,n+1);
}
template<class Api> BindingResult BindAndLoad(Api& api, BindingState& state,
        Address base, Address scratch, const CallbackAddresses& callbacks,
        const char* absoluteSupplementPath) {
    const auto finish=[&](BindingResult result,bool fatal=false){
        state.Last=result;if(fatal)state.Stage=BindingStage::Failed;return result;
    };
    if(state.Stage==BindingStage::Ready)return BindingResult::Ready;
    if(state.Stage==BindingStage::Failed)return state.Last;
    if(!base||scratch<4096||scratch>UINT32_MAX-2048||
       !callbacks.Construct||!absoluteSupplementPath)
        return finish(BindingResult::ApiFailure,true);
    std::uint32_t initialized=0;
    if(!BindingCall(api,base+0x01000AE0,nullptr,0,initialized))
        return finish(BindingResult::ApiFailure);
    if(initialized!=1)return finish(BindingResult::Waiting);
    Address parent=0,within=0;
    if(Read(api,base+0x03006EE4,parent)!=Status::Ok ||
       Read(api,base+0x02FFA36C,within)!=Status::Ok)
        return finish(BindingResult::ApiFailure);
    if(!parent||!within)return finish(BindingResult::Waiting);
    std::uint32_t extent=0;
    if(Read(api,parent+0x48,extent)!=Status::Ok)
        return finish(BindingResult::ApiFailure);
    state.BaseExtent=extent&0xFFFFFF;
    if(state.BaseExtent!=0xC5)return finish(BindingResult::WrongBaseLayout,true);
    state.Native.Base=base;state.Native.BaseClass=parent;
    state.Native.Vtable=scratch+8;
    if(BuildVtable(api,state.Native,callbacks)!=Status::Ok)
        return finish(BindingResult::ApiFailure,true);
    constexpr Address nameOff=0x240,packageOff=0x270,configOff=0x280;
    constexpr Address fullNameOff=0x290,pathOff=0x300;
    if(!BindingString(api,scratch+nameOff,"WillowAnimNodeBlendWheeledPawn",48)||
       !BindingString(api,scratch+packageOff,"WillowGame",16)||
       !BindingString(api,scratch+configOff,"Engine",16)||
       !BindingString(api,scratch+fullNameOff,"WillowGame.WillowAnimNodeBlendWheeledPawn",96)||
       !BindingString(api,scratch+pathOff,absoluteSupplementPath,1024))
        return finish(BindingResult::ApiFailure,true);
    std::uint32_t found=0;
    const std::uint32_t findWords[]={0,UINT32_MAX,scratch+fullNameOff,0};
    if(!BindingCall(api,base+0x01007DAC,findWords,4,found))
        return finish(BindingResult::ApiFailure,true);
    if(found)return finish(BindingResult::ExistingClass,true);
    // Allocate exactly as an original generated GetPrivateStaticClass caller:
    // engine GMalloc virtual Malloc(size0x168, alignment8), not host scratch.
    Address allocator=0,table=0,mallocFn=0;
    if(Read(api,base+0x02F9132C,allocator)!=Status::Ok||
       Read(api,allocator,table)!=Status::Ok||
       Read(api,table+4,mallocFn)!=Status::Ok)
        return finish(BindingResult::ApiFailure,true);
    const std::uint32_t mallocWords[]={allocator,0x168,8};
    if(!BindingCall(api,mallocFn,mallocWords,3,state.Descriptor)||!state.Descriptor)
        return finish(BindingResult::ApiFailure,true);
    state.Native.NativeClass=state.Descriptor;
    const std::uint32_t ctorWords[]={
        state.Descriptor,0,0x100,0,0,
        scratch+nameOff,scratch+packageOff,scratch+configOff,
        0x00004000,0x04084084,callbacks.Construct,
        base+0x00FF94D8,0,base+0x00FF94DC,0
    };
    std::uint32_t returned=0;
    if(!BindingCall(api,base+0x00F5D9C8,ctorWords,15,returned)||returned!=state.Descriptor)
        return finish(BindingResult::WrongConstructorState,true);
    // Original generated caller sets the native instance alignment byte16.
    const std::uint8_t alignment=16;
    if(Write(api,state.Descriptor+0x4B,alignment)!=Status::Ok)
        return finish(BindingResult::WrongConstructorState,true);
    Address nativeCtor=0;
    if(Read(api,state.Descriptor+0xF4,nativeCtor)!=Status::Ok||nativeCtor!=callbacks.Construct)
        return finish(BindingResult::WrongConstructorState,true);
    // EStaticConstructor marks compiled-in objects DisregardForGC. This
    // descriptor is registered after GetInitialized, while its new metadata
    // and CDO are ordinary GC objects. Keep the owned descriptor in that same
    // GC graph before original AddObject assigns its immutable ObjectIndex.
    // Never alter a registered object, ClassFlags, GC globals or object index.
    std::uint32_t objectIndex=0,objectFlagsLo=0,objectFlagsHi=0,linker=0,linkerIndex=0;
    if(Read(api,state.Descriptor+0x20,objectIndex)!=Status::Ok||objectIndex!=UINT32_MAX||
       Read(api,state.Descriptor+8,objectFlagsLo)!=Status::Ok||objectFlagsLo!=0x00004000||
       Read(api,state.Descriptor+0xC,objectFlagsHi)!=Status::Ok||objectFlagsHi!=0x04084084||
       Read(api,state.Descriptor+0x18,linker)!=Status::Ok||linker!=0||
       Read(api,state.Descriptor+0x1C,linkerIndex)!=Status::Ok||linkerIndex!=UINT32_MAX)
        return finish(BindingResult::WrongConstructorState,true);
    const std::uint32_t dynamicFlagsHi=objectFlagsHi&~0x80u;
    if(Write(api,state.Descriptor+0xC,dynamicFlagsHi)!=Status::Ok||
       Read(api,state.Descriptor+0xC,objectFlagsHi)!=Status::Ok||objectFlagsHi!=dynamicFlagsHi)
        return finish(BindingResult::WrongConstructorState,true);
    state.Stage=BindingStage::Constructed;
    const std::uint32_t initWords[]={parent,state.Descriptor,within};
    if(!BindingCall(api,base+0x00F60C08,initWords,3,returned))
        return finish(BindingResult::RegistrationFailed,true);
    Address actualParent=0,actualWithin=0,actualClass=0;
    if(Read(api,state.Descriptor+0x40,actualParent)!=Status::Ok||actualParent!=parent||
       Read(api,state.Descriptor+0xCC,actualWithin)!=Status::Ok||actualWithin!=within||
       Read(api,state.Descriptor+0x34,actualClass)!=Status::Ok||!actualClass||
       !BindingCall(api,base+0x01007DAC,findWords,4,found)||found!=state.Descriptor)
        return finish(BindingResult::RegistrationFailed,true);
    std::uint32_t firstGcIndex=0;
    if(Read(api,base+0x02FFA2A8,firstGcIndex)!=Status::Ok||
       Read(api,state.Descriptor+0x20,objectIndex)!=Status::Ok||
       objectIndex==UINT32_MAX||firstGcIndex==UINT32_MAX||objectIndex<firstGcIndex||
       Read(api,state.Descriptor+0xC,objectFlagsHi)!=Status::Ok||(objectFlagsHi&0x80u)!=0)
        return finish(BindingResult::RegistrationFailed,true);
    state.Stage=BindingStage::Registered;
    const std::uint32_t loadWords[]={0,scratch+pathOff,0};
    if(!BindingCall(api,base+0x0100D228,loadWords,3,state.Package)||!state.Package)
        return finish(BindingResult::PackageLoadFailed,true);
    // Verify actual original reflection linking, not the host wire parser.
    constexpr std::array<Address,13> offsets{
        0xC8,0xD0,0xD8,0xDC,0xE0,0xE4,0xE8,0xEC,0xF0,0xF0,0xF0,0xF4,0xF8};
    Address property=0;
    if(Read(api,state.Descriptor+0x44,property)!=Status::Ok)
        return finish(BindingResult::WrongLinkedProperties,true);
    for(unsigned i=0;i<offsets.size();++i){
        std::uint32_t offset=0,mask=0;
        if(!property||Read(api,property+0x54,offset)!=Status::Ok||offset!=offsets[i])
            return finish(BindingResult::WrongLinkedProperties,true);
        if(i>=8&&i<=10&&
           (Read(api,property+0x74,mask)!=Status::Ok||mask!=(1u<<(i-8))))
            return finish(BindingResult::WrongLinkedProperties,true);
        if(Read(api,property+0x3C,property)!=Status::Ok)
            return finish(BindingResult::WrongLinkedProperties,true);
    }
    if(Read(api,state.Descriptor+0x48,extent)!=Status::Ok)
        return finish(BindingResult::WrongLinkedProperties,true);
    state.LinkedExtent=extent&0xFFFFFF;
    if(state.LinkedExtent<0xFC||state.LinkedExtent>0x100)
        return finish(BindingResult::WrongLinkedProperties,true);
    Address cdoClass=0,cdoTable=0;
    if(Read(api,state.Descriptor+0xF0,state.DefaultObject)!=Status::Ok||!state.DefaultObject||
       Read(api,state.DefaultObject+0x34,cdoClass)!=Status::Ok||cdoClass!=state.Descriptor||
       Read(api,state.DefaultObject,cdoTable)!=Status::Ok||cdoTable!=state.Native.Vtable||
       Read(api,state.Descriptor+0xF4,nativeCtor)!=Status::Ok||nativeCtor!=callbacks.Construct)
        return finish(BindingResult::WrongDefaultObject,true);
    state.Stage=BindingStage::Ready;
    return finish(BindingResult::Ready);
}
} // namespace wheeled_pawn_arm_candidate

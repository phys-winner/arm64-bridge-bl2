#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif
#include "bl2_iris_wheeled_math.h"

// Isolated, unregistered implementation candidate. The caller owns verified
// class registration, metadata/CDO linkage, thunk registration and guest memory.
// Api::Call must complete synchronously, return false on deferral/failure, and
// expose the original soft-float ARM return word. No failed call is success.
namespace wheeled_pawn_arm_candidate {
using Address = std::uint32_t;
struct Runtime {
    Address Base = 0;
    Address NativeClass = 0;
    Address BaseClass = 0;
    Address Vtable = 0;
};
struct CallbackAddresses {
    Address Construct = 0, ParentChanged = 0, Relevant = 0, Tick = 0, Context = 0;
};
enum class Status {
    Ok, ReadFailed, WriteFailed, CallFailed, ClassUnavailable, WrongClass,
    ClassCycle, WrongChildren, InvalidAddress
};
inline std::uint32_t FloatWord(float value) {
    std::uint32_t word; std::memcpy(&word, &value, sizeof(word)); return word;
}
inline float WordFloat(std::uint32_t word) {
    float value; std::memcpy(&value, &word, sizeof(value)); return value;
}
template<class Api, class T> Status Read(Api& api, Address p, T& out) {
    if (p < 4096 || sizeof(T) > std::numeric_limits<Address>::max() - p)
        return Status::InvalidAddress;
    return api.Read(p, &out, sizeof(out)) ? Status::Ok : Status::ReadFailed;
}
template<class Api, class T> Status Write(Api& api, Address p, const T& value) {
    if (p < 4096 || sizeof(T) > std::numeric_limits<Address>::max() - p)
        return Status::InvalidAddress;
    return api.Write(p, &value, sizeof(value)) ? Status::Ok : Status::WriteFailed;
}
template<class Api> Status Call(Api& api, Address fn,
        const std::uint32_t* args, std::size_t count, std::uint32_t& result) {
    if (fn < 4096 || count > 7) return Status::InvalidAddress;
    return api.Call(fn, args, count, result) ? Status::Ok : Status::CallFailed;
}
template<class Api> Status BuildVtable(Api& api, const Runtime& rt,
        const CallbackAddresses& callbacks) {
    if (!rt.Base || rt.Vtable < 8 || !callbacks.Construct ||
        !callbacks.ParentChanged || !callbacks.Relevant || !callbacks.Tick ||
        !callbacks.Context) return Status::ClassUnavailable;
    // Original base is552B including its two-word Itanium header. Add the
    // subclass-only getter at ARM addresspoint+220; PC+224 is a different ABI.
    std::array<Address,139> words{};
    if (!api.Read(rt.Base+0x02D76CA0,words.data(),552))return Status::ReadFailed;
    if(words[(8+0x164)/4]!=rt.Base+0x01328F14 ||
       words[(8+0x17C)/4]!=rt.Base+0x0132938C ||
       words[(8+0x1A4)/4]!=rt.Base+0x0132A9D8 ||
       words[(8+0x21C)/4]!=rt.Base+0x01327F9C) return Status::WrongClass;
    words[(8+4)/4]=callbacks.ParentChanged;
    words[(8+0x17C)/4]=callbacks.Tick;
    words[(8+0x1A4)/4]=callbacks.Relevant;
    words[(8+0x220)/4]=callbacks.Context;
    return api.Write(rt.Vtable-8,words.data(),sizeof(words))?
        Status::Ok:Status::WriteFailed;
}
template<class Api> Status ClassPointer(Api& api, const Runtime& rt,
        Address rva, Address& result) {
    auto status = Read(api, rt.Base + rva, result);
    if (status != Status::Ok) return status;
    return result ? Status::Ok : Status::ClassUnavailable;
}
template<class Api> Status IsA(Api& api, Address object, Address expected, bool& yes) {
    yes = false;
    if (!object) return Status::Ok;
    if (!expected) return Status::ClassUnavailable;
    Address cls = 0;
    auto status = Read(api, object + 0x34, cls);
    if (status != Status::Ok) return status;
    // A corrupt or cyclic chain must not become a successful cast.
    std::array<Address, 64> visited{};
    for (std::size_t depth = 0; cls && depth < visited.size(); ++depth) {
        if (cls == expected) { yes = true; return Status::Ok; }
        for (std::size_t i = 0; i < depth; ++i)
            if (visited[i] == cls) return Status::ClassCycle;
        visited[depth] = cls;
        status = Read(api, cls + 0x40, cls);
        if (status != Status::Ok) return status;
    }
    return cls ? Status::ClassCycle : Status::Ok;
}
template<class Api> Status CheckNode(Api& api, const Runtime& rt, Address node) {
    if (!rt.Base || !rt.NativeClass || !rt.BaseClass || !rt.Vtable)
        return Status::ClassUnavailable;
    bool yes = false;
    auto status = IsA(api, node, rt.NativeClass, yes);
    if (status != Status::Ok) return status;
    return yes ? Status::Ok : Status::WrongClass;
}
template<class Api> Status Context(Api& api, Address node, Address& result) {
    result = 0;
    Address component = 0;
    auto status = Read(api, node + 0x3C, component);
    if (status != Status::Ok || !component) return status;
    status = Read(api, component + 0x50, result);
    if (status != Status::Ok || !result) return status;
    Address base = 0; std::uint8_t flags = 0;
    status = Read(api, result + 0xA8, base);
    if (status != Status::Ok) return status;
    status = Read(api, node + 0xF0, flags);
    if (status == Status::Ok && base && (flags & 1)) result = base;
    return status;
}
template<class Api> Status Virtual0(Api& api, Address object, Address slot,
        std::uint32_t& result) {
    Address table = 0, method = 0;
    auto status = Read(api, object, table);
    if (status != Status::Ok) return status;
    status = Read(api, table + slot, method);
    if (status != Status::Ok) return status;
    return Call(api, method, &object, 1, result);
}
template<class Api> Status MovementActive(Api& api, const Runtime& rt,
        Address node, Address context, bool& active) {
    active = false;
    Address aiClass = 0;
    auto status = ClassPointer(api, rt, 0x03067A98, aiClass);
    if (status != Status::Ok) return status;
    bool ai = false;
    status = IsA(api, context, aiClass, ai);
    if (status != Status::Ok) return status;
    if (ai) {
        Address world = 0; std::uint32_t mode = 0;
        status = Read(api, rt.Base + 0x0300D940, world);
        if (status != Status::Ok) return status;
        status = Call(api, rt.Base + 0x017BA4D4, &world, 1, mode);
        if (status != Status::Ok) return status;
        std::uint8_t movement = 0;
        status = Read(api, context + 0x780, movement);
        if (status != Status::Ok) return status;
        if (mode != 3) {
            std::uint8_t force = 0, charging = 0;
            status = Read(api, context + 0x781, force);
            if (status != Status::Ok) return status;
            movement &= static_cast<std::uint8_t>(~0x80u);
            status = Write(api, context + 0x780, movement);
            if (status != Status::Ok) return status;
            if (!(force & 1)) {
                status = Read(api, context + 0x10CD, charging);
                if (status != Status::Ok) return status;
                bool following = (charging & 8) != 0;
                if (!following) {
                    Address mind = 0, mindClass = 0;
                    status = Read(api, context + 0x1B0, mind);
                    if (status != Status::Ok) return status;
                    status = ClassPointer(api, rt, 0x0306B4A4, mindClass);
                    if (status != Status::Ok) return status;
                    bool isMind = false;
                    status = IsA(api, mind, mindClass, isMind);
                    if (status != Status::Ok) return status;
                    if (isMind) {
                        Address navigation = 0;
                        status = Read(api, mind + 0x7A4, navigation);
                        if (status != Status::Ok) return status;
                        if (navigation) {
                            std::uint32_t answer = 0;
                            status = Virtual0(api, navigation, 0x188, answer);
                            if (status != Status::Ok) return status;
                            if (answer) {
                                status = Virtual0(api, navigation, 0x1C0, answer);
                                if (status != Status::Ok) return status;
                                following = answer != 0;
                            }
                        }
                    }
                }
                if (following) {
                    status = Read(api, context + 0x780, movement);
                    if (status != Status::Ok) return status;
                    movement |= 0x80;
                    status = Write(api, context + 0x780, movement);
                    if (status != Status::Ok) return status;
                }
            }
            std::uint8_t dirty = 0;
            status = Read(api, context + 0xBA, dirty);
            if (status != Status::Ok) return status;
            dirty |= 8;
            status = Write(api, context + 0xBA, dirty);
            if (status != Status::Ok) return status;
        }
        // Preserve source re-read after virtual side effects.
        status = Read(api, context + 0x780, movement);
        if (status != Status::Ok) return status;
        active = (movement & 0x80) != 0;
    }
    std::uint8_t flags = 0;
    status = Read(api, node + 0xF0, flags);
    if (status != Status::Ok) return status;
    if (active) flags &= static_cast<std::uint8_t>(~4u);
    else if (!(flags & 4)) { flags |= 4; active = true; }
    return Write(api, node + 0xF0, flags);
}
template<class Api> Status Planar(Api& api, const Runtime& rt,
        Address pitchAddress, Address yawAddress, wheeled_pawn_candidate::Vector2& out) {
    std::uint32_t pitch = 0, yaw = 0;
    auto status = Read(api, pitchAddress, pitch);
    if (status != Status::Ok) return status;
    status = Read(api, yawAddress, yaw);
    if (status != Status::Ok) return status;
    // Unsigned shift has the same low14 bits as source SAR for this mask.
    const auto table = rt.Base + 0x02F9FF50;
    float cosPitch = 0, cosYaw = 0, sinYaw = 0;
    status = Read(api, table + 4 * (((pitch + 16384u) >> 2) & 16383u), cosPitch);
    if (status != Status::Ok) return status;
    status = Read(api, table + 4 * (((yaw + 16384u) >> 2) & 16383u), cosYaw);
    if (status != Status::Ok) return status;
    status = Read(api, table + 4 * ((yaw >> 2) & 16383u), sinYaw);
    if (status != Status::Ok) return status;
    out = {cosYaw * cosPitch, sinYaw * cosPitch};
    return Status::Ok;
}
template<class Api> Status Construct(Api& api, const Runtime& rt, Address node) {
    if (!rt.Base || !rt.Vtable) return Status::ClassUnavailable;
    std::uint32_t ignored = 0;
    auto status = Call(api, rt.Base + 0x010160F0, &node, 1, ignored);
    if (status != Status::Ok) return status;
    return Write(api, node, rt.Vtable);
}
template<class Api> Status HasParentClassChanged(Api& api, const Runtime& rt,
        Address node, std::uint32_t& changed) {
    Address cls = 0, super = 0;
    auto status = Read(api, node + 0x34, cls);
    if (status != Status::Ok) return status;
    status = Read(api, cls + 0x40, super);
    if (status != Status::Ok) return status;
    if (!rt.BaseClass) return Status::ClassUnavailable;
    changed = super != rt.BaseClass;
    return Status::Ok;
}
template<class Api> Status OnBecomeRelevant(Api& api, const Runtime& rt, Address node) {
    auto status = CheckNode(api, rt, node);
    if (status != Status::Ok) return status;
    std::uint32_t ignored = 0;
    status = Call(api, rt.Base + 0x0132A9D8, &node, 1, ignored);
    if (status != Status::Ok) return status;
    Address context = 0;
    status = Context(api, node, context);
    if (status != Status::Ok || !context) return status;
    float x = 0, y = 0, z = 0;
    status = Read(api, context + 0x104, x);
    if (status != Status::Ok) return status;
    status = Read(api, context + 0x108, y);
    if (status != Status::Ok) return status;
    status = Read(api, context + 0x10C, z);
    if (status != Status::Ok) return status;
    const float xx = x*x, yy = y*y, zz = z*z, xy = xx+yy;
    const float speed = std::sqrt(xy+zz);
    return Write(api, node + 0xF8, speed);
}
template<class Api> Status Tick(Api& api, const Runtime& rt, Address node, float dt) {
    using namespace wheeled_pawn_candidate;
    auto status = CheckNode(api, rt, node);
    if (status != Status::Ok) return status;
    Address children = 0; std::int32_t count = 0, capacity = 0;
    status = Read(api, node + 0xB4, children);
    if (status != Status::Ok) return status;
    status = Read(api, node + 0xB8, count);
    if (status != Status::Ok) return status;
    status = Read(api, node + 0xBC, capacity);
    if (status != Status::Ok) return status;
    if (!children || count != 4 || capacity < 4 || capacity > 4096)
        return Status::WrongChildren;
    Config config{}; State state{}; Inputs input{};
    input.DeltaTime=dt;
    Address context=0, pawnClass=0;
    status = Context(api,node,context);
    if (status != Status::Ok) return status;
    status = ClassPointer(api,rt,0x0306C200,pawnClass);
    if (status != Status::Ok) return status;
    status = IsA(api,context,pawnClass,input.HasWillowPawnContext);
    if (status != Status::Ok) return status;
    if (input.HasWillowPawnContext) {
        status = MovementActive(api,rt,node,context,input.MovementActive);
        if (status != Status::Ok) return status;
        if (input.MovementActive) {
            std::uint32_t scale=0,speed=0;
            status = Virtual0(api,context,0x68C,scale);
            if (status != Status::Ok) return status;
            status = Call(api,rt.Base+0x015E7CA4,&context,1,speed);
            if (status != Status::Ok) return status;
            input.TargetSpeed=WordFloat(scale)*WordFloat(speed);
        }
    }
    // Read numeric state after original native context/navigation/speed calls,
    // matching the source order rather than caching it across guest callbacks.
    std::array<float,12> scalars{};
    status = Read(api, node + 0xC8, scalars);
    if (status != Status::Ok) return status;
    config.DirChangeRateMin = {scalars[0],scalars[1]};
    config.DirChangeRateMax = {scalars[2],scalars[3]};
    config.BlendTime=scalars[4]; config.ReferenceSpeed=scalars[5];
    config.SpeedUpRate=scalars[6]; config.SpeedDownRate=scalars[7];
    config.BrakeRate=scalars[8]; config.IdleBlendTime=scalars[9];
    // F0 is a packed bitfield, not one of the float config values.
    state.DirAngle=scalars[11];
    status = Read(api, node + 0xF8, state.CurSpeed);
    if (status != Status::Ok) return status;
    std::uint8_t flags = 0;
    status = Read(api, node + 0xF0, flags);
    if (status != Status::Ok) return status;
    state.bIsIdle=(flags&2)!=0;
    std::array<Address,4> sequences{};
    for (unsigned i=0;i<4;++i) {
        status = Read(api, children+i*0x1C+0x0C, state.Weight[i]);
        if (status != Status::Ok) return status;
        status = Read(api, children+i*0x1C+8, sequences[i]);
        if (status != Status::Ok) return status;
    }
    if (input.HasWillowPawnContext) {
        Address sequenceClass=0;
        if (config.ReferenceSpeed!=0) {
            status=ClassPointer(api,rt,0x03006ED8,sequenceClass);
            if(status!=Status::Ok)return status;
            for(unsigned i=1;i<4;++i){
                status=IsA(api,sequences[i],sequenceClass,input.SequenceCast[i]);
                if(status!=Status::Ok)return status;
            }
        }
        status=Planar(api,rt,context+0x45C,context+0x460,input.DesiredPlanar);
        if(status!=Status::Ok)return status;
        status=Planar(api,rt,context+0x6C,context+0x70,input.ActorPlanar);
        if(status!=Status::Ok)return status;
    }
    const auto result=wheeled_pawn_candidate::Tick(state,config,input);
    if(input.HasWillowPawnContext){
        status=Write(api,node+0xF8,result.state.CurSpeed);
        if(status!=Status::Ok)return status;
        // MovementActive may have changed the source one-frame flag.
        status=Read(api,node+0xF0,flags);
        if(status!=Status::Ok)return status;
        flags=(flags&static_cast<std::uint8_t>(~2u))|(result.state.bIsIdle?2:0);
        status=Write(api,node+0xF0,flags);
        if(status!=Status::Ok)return status;
    }
    status=Write(api,node+0xF4,result.state.DirAngle);
    if(status!=Status::Ok)return status;
    for(unsigned i=1;i<4;++i)if(result.SetPlayRate[i]){
        status=Write(api,sequences[i]+0xBC,result.PlayRate);
        if(status!=Status::Ok)return status;
    }
    for(unsigned i=0;i<4;++i){
        status=Write(api,children+i*0x1C+0x0C,result.state.Weight[i]);
        if(status!=Status::Ok)return status;
    }
    std::uint32_t ignored=0;
    const std::uint32_t args[]={node,FloatWord(dt)};
    return Call(api,rt.Base+0x0132938C,args,2,ignored);
}
} // namespace wheeled_pawn_arm_candidate

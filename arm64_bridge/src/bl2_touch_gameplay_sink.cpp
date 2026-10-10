#include "bl2_touch_gameplay_sink.h"
#include "bl2_touch_physical.h"
#include "bl2_touch_gfx_transport.h"
#include "bl2_touch_widget_registry.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "bl2_touch_guest_call.h"
#include "elf_loader.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_bindings.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace bl2::touch {
namespace {
constexpr std::uint32_t kAvailability = 0x0235679C;
constexpr std::uint32_t kFireBehaviors = 0x0236D8FC;
constexpr std::uint32_t kAxisPointer = 0x0236E210;
constexpr std::uint32_t kAddAxis = 0x01821D6C;
constexpr std::uint32_t kGetHudMovie = 0x0235734C;
constexpr std::uint32_t kCountReadied = 0x0229ECA0;
constexpr std::uint32_t kActorProcessEvent = 0x010E86C8;
constexpr std::uint32_t kAppFree = 0x00F26444;
constexpr std::uint32_t kGetActionSkill = 0x0207B904;
constexpr float kRotatorUnitsPerDegree = 65536.0f / 360.0f;

std::uint32_t floatWord(float value) {
    std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); return bits;
}
float wordFloat(std::uint32_t bits) {
    float value = 0; std::memcpy(&value, &bits, sizeof(value)); return value;
}
std::uint64_t mix(std::uint64_t seed, std::uint64_t value) {
    return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
}
struct Target {
    std::uint32_t controller = 0, input = 0, world = 0, viewport = 0;
    std::uint64_t controllerToken = 0, inputToken = 0;
    std::uint64_t scene = 0, generation = 0, fingerprint = 0;
};
struct Definition {
    std::uint32_t object = 0;
    std::uint64_t token = 0;
    bool always = false;
};
struct Property {
    std::uint32_t object = 0, offset = 0, size = 0, extra = 0, dimension = 0;
    explicit operator bool() const { return object != 0; }
};
struct OwnedAction {
    Target target{};
    Definition definition{};
    std::uint64_t epoch = 0;
    bool held = false;
};
} // namespace

struct Ue3GameplaySink::State {
    DynarmicHost& host;
    LifetimeGeneration generation;
    const std::uint32_t base;
    std::uint32_t arrayScratch = 0;
    std::uint32_t queryScratch = 0;
    Target prepared{}, frameTarget{};
    std::array<OwnedAction, kActionCount> owned{};
    std::uint64_t frameEpoch = 0, available = 0;
    Context observation{};
    struct HudSource {
        Target target{};
        std::uint64_t owner = 0;
        std::uint32_t movie = 0, interface = 0, viewport = 0, width = 0, height = 0;
        bool armed = false, usable = false;
    } hudSource{};
    Context hudObservation{};
    std::uint64_t hudSampleTime = 0;
    struct AvailabilityCache {
        Target target{};
        std::uint64_t pawn = 0, time = 0;
        bool known = false, usable = false;
#if HORN_ENABLE_LOGGING
        const char* reason = "use-trigger-unobserved";
#endif
    } useCache{},skillCache{};
    std::uint64_t skillTreeToken = 0;
    bool triggerQueriesDisabled = false;
    std::unique_ptr<GfxHostTransport> hudTransport;
#if HORN_ENABLE_LOGGING
    Context lastDiagnostic{};
    std::uint64_t lastDiagnosticOwner = 0;
    const char* hudReason = "unobserved";
    const char* objectiveReason = "objective-unobserved";
    const char* useReason = "use-unobserved";
    const char* skillReason = "skill-unobserved";
    std::array<double,7> ammoMetrics{};
    std::uint64_t diagnosticTime = 0;
    unsigned diagnosticLogs = 0;
    unsigned queryDiagnosticLogs = 0;
    struct HintDiagnostic {
        std::uint64_t owner=0;
        const char* reason=nullptr;
        double frame=-1;
        SurfaceRect rect{};
    };
    std::array<HintDiagnostic,2> lastHintDiagnostic{};
    unsigned hintDiagnosticLogs=0;
#endif
    EquippedWeaponHistory weaponHistory{};
    struct CachedProperty { std::uint64_t type = 0; const char* name = nullptr; const char* kind = nullptr; Property value{}; };
    mutable std::vector<CachedProperty> properties;
    struct CachedFunction { std::uint64_t type = 0; const char* name = nullptr; const char* owner = nullptr; std::uint32_t value = 0; std::uint64_t identity = 0; };
    mutable std::vector<CachedFunction> functions;
    bool preparedValid = false, frameValid = false;
    bool movementPending = false, lookPending = false;
    float right = 0, forward = 0, yaw = 0, pitch = 0;

    State(DynarmicHost& value, LifetimeGeneration callback)
        : host(value), generation(callback), base(value.GetElfLoader().GetBaseAddress()) {}
    ~State() { if (arrayScratch) host.GuestFree(arrayScratch); if (queryScratch) host.GuestFree(queryScratch); }
    bool span(std::uint32_t p, std::size_t size) const {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() && size <= host.GetGuestMemorySize() - p;
    }
    std::uint32_t read(std::uint32_t p) const { return span(p, 4) ? host.MemoryRead32(p) : 0; }
    bool array(std::uint32_t p, std::uint32_t limit, std::uint32_t& data, std::uint32_t& count) const {
        if (!span(p, 12)) return false;
        data = read(p); count = read(p + 4);
        const auto capacity = read(p + 8);
        return count <= capacity && capacity <= limit && (!count || span(data, std::size_t(count) * 4));
    }
    std::uint64_t token(std::uint32_t object) const {
        std::uint32_t data = 0, count = 0;
        if (!span(object, 0x38) || (read(object + 0x0C) & 0x20000002u) ||
            !array(base + 0x02FBA268, 2000000, data, count)) return 0;
        const auto index = read(object + 0x20);
        if (index >= count || read(data + index * 4) != object) return 0;
        return (std::uint64_t(object) << 32) | index;
    }
    bool isWillowInput(std::uint32_t input) const {
        auto cls = read(input + 0x34);
        const auto willowClass = read(base + 0x0306C578);
        if (!token(input) || !token(willowClass)) return false;
        for (int i = 0; i != 48 && cls; ++i) {
            if (!token(cls) || !span(cls, 0x44)) return false;
            if (cls == willowClass) return true;
            cls = read(cls + 0x40);
        }
        return false;
    }
    std::string name(std::uint32_t address) const {
        std::uint32_t data = 0, count = 0;
        if (!span(address, 8) || !array(base + 0x02FB00C8, 2000000, data, count)) return {};
        const auto index = read(address);
        if (index >= count) return {};
        const auto entry = read(data + index * 4);
        if (!span(entry, 136)) return {};
        std::string value;
        for (std::uint32_t i = 0; i != 128; ++i) {
            const auto c = host.MemoryRead8(entry + 8 + i);
            if (!c) {
                const auto number = read(address + 4);
                if (number) value += "_" + std::to_string(number - 1);
                return value;
            }
            if (c < 32 || c > 126) return {};
            value += static_cast<char>(c);
        }
        return {};
    }
    std::string objectName(std::uint32_t object) const {
        return token(object) ? name(object + 0x2C) : std::string{};
    }
    std::string className(std::uint32_t object) const { return objectName(read(object + 0x34)); }
    bool isA(std::uint32_t object, const char* wanted) const {
        if (!token(object)) return false;
        auto type = read(object + 0x34);
        for (unsigned i = 0; type && i != 48; ++i) {
            if (!token(type) || className(type) != "Class") return false;
            if (objectName(type) == wanted) return true;
            type = read(type + 0x40);
        }
        return false;
    }
    // Native UProperty metadata provides offsets for this exact loaded class;
    // no inventory or pawn address is assumed from a different UE3 release.
    Property property(std::uint32_t type, const char* wanted, const char* kind) const {
        const auto requested = token(type);
        if (!requested) return {};
        for (const auto& cached : properties)
            if (cached.type == requested && std::strcmp(cached.name, wanted) == 0 &&
                std::strcmp(cached.kind, kind) == 0 && token(cached.value.object)) return cached.value;
        for (unsigned depth = 0; type && depth != 48; ++depth) {
            if (!token(type)) return {};
            const auto total = read(type + 0x48) & 0x00FFFFFFu;
            if (!total || total > 1048576) return {};
            auto field = read(type + 0x44);
            for (unsigned i = 0; field && i != 2048; ++i) {
                if (!token(field) || read(field + 0x28) != type) return {};
                if (objectName(field) == wanted) {
                    Property p{field, read(field + 0x54), read(field + 0x44), read(field + 0x74), read(field + 0x40)};
                    if (className(field) != kind || !p.dimension || p.dimension > 16 || !p.size ||
                        p.size > 4096 || p.offset > total || p.size * p.dimension > total - p.offset) return {};
                    if (properties.size() >= 64) properties.clear();
                    properties.push_back({requested, wanted, kind, p});
                    return p;
                }
                field = read(field + 0x3C);
            }
            type = read(type + 0x40);
        }
        return {};
    }
    Property field(std::uint32_t object, const char* wanted, const char* kind) const {
        return token(object) ? property(read(object + 0x34), wanted, kind) : Property{};
    }
    bool objectField(std::uint32_t object, const char* wanted, std::uint32_t& out) const {
        const auto p = field(object, wanted, "ObjectProperty");
        if (!p || p.size != 4 || p.dimension != 1 || !span(object + p.offset, 4)) return false;
        out = read(object + p.offset);
        return !out || token(out);
    }
    std::uint32_t function(std::uint32_t object, const char* wanted, const char* owner) const {
        auto type = read(object + 0x34);
        const auto requested = token(type);
        if (!token(object) || !requested) return 0;
        for (const auto& cached : functions)
            if (cached.type == requested && std::strcmp(cached.name, wanted) == 0 &&
                std::strcmp(cached.owner, owner) == 0 && token(cached.value) == cached.identity) return cached.value;
        for (unsigned depth = 0; type && depth != 48; ++depth) {
            if (!token(type)) return 0;
            auto item = read(type + 0x44);
            for (unsigned i = 0; item && i != 2048; ++i) {
                if (!token(item) || read(item + 0x28) != type) return 0;
                if (objectName(item) == wanted) {
                    if (className(item) != "Function" || objectName(type) != owner || !span(item, 0xA0)) return 0;
                    if (functions.size() >= 16) functions.clear();
                    functions.push_back({requested, wanted, owner, item, token(item)});
                    return item;
                }
                item = read(item + 0x3C);
            }
            type = read(type + 0x40);
        }
        return 0;
    }
#if HORN_ENABLE_LOGGING
    void queryDiagnostic(const char* name,std::uint32_t fn,const Property& a,const Property& b) {
        if (queryDiagnosticLogs++>=8) return;
        const auto layout=read(fn+0x84);
        HORN_LOGI("BL2TouchDiagnostics","gameplay query rejected name=%s fn=%08X flags=%08X parms=%u bytes=%u ret=%u "
            "a=(%08X,%u,%u,%u,%08X,%08X) b=(%08X,%u,%u,%u,%08X,%08X)",name,fn,read(fn+0x7C),
            (layout>>8)&255,layout>>16,read(fn+0x88)&65535,
            a.object,a.offset,a.size,a.dimension,a.extra,read(a.object+0x48),
            b.object,b.offset,b.size,b.dimension,b.extra,read(b.object+0x48));
    }
#endif
    bool boolQuery(const Target& target, std::uint32_t object, const char* method,
                   const char* owner, std::uint32_t flags, bool& result) {
        const auto fn = function(object, method, owner);
        const auto value = fn ? property(fn, "ReturnValue", "BoolProperty") : Property{};
        if (!fn || !value || value.offset || value.size != 4 || value.dimension != 1 || value.extra != 1 ||
            read(fn + 0x7C) != flags || host.MemoryRead8(fn + 0x85) != 1 ||
            host.MemoryRead16(fn + 0x86) != 4 || host.MemoryRead16(fn + 0x88) != 0 ||
            (host.MemoryRead64(value.object + 0x48) & 0x580u) != 0x580u ||
            read(read(object) + 0x10C) != base + kActorProcessEvent || !targetUnchanged(target)) {
#if HORN_ENABLE_LOGGING
            queryDiagnostic(method,fn,value,{});
#endif
            return false;
        }
        const auto identity = token(object), functionIdentity = token(fn);
        if (!queryScratch) queryScratch = host.GuestAlloc(32, 8);
        if (!queryScratch) return false;
        host.MemoryWrite32(queryScratch, 0xFFFFFFFFu);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base + kActorProcessEvent, {object, fn, queryScratch, 0});
#else
        if (!Bl2TouchCallGuest(host, base + kActorProcessEvent, {object, fn, queryScratch, 0})) return false;
#endif
        const auto word = read(queryScratch);
        if (!targetUnchanged(target) || token(object) != identity || token(fn) != functionIdentity || word > 1) return false;
        result = word != 0;
        return true;
    }
    void observeReload(const Target& target, std::uint32_t weapon) {
        const auto count = field(weapon, "ReloadCnt", "IntProperty");
        const auto capacity = field(weapon, "ClipSize", "IntAttributeProperty");
        if (!count || !capacity || count.size != 4 || capacity.size != 4 ||
            count.dimension != 1 || capacity.dimension != 1 ||
            !span(weapon + count.offset, 4) || !span(weapon + capacity.offset, 4)) return;
        const auto loaded = static_cast<std::int32_t>(read(weapon + count.offset));
        const auto maximum = static_cast<std::int32_t>(read(weapon + capacity.offset));
        if (loaded < 0 || loaded > 65535 || maximum < 1 || maximum > 65535 || !targetUnchanged(target)) return;
        // BeginReload compares the current ReloadCnt with the effective ClipSize.
        // execInstanceAttribute reads this IntAttributeProperty through the
        // ordinary UIntProperty::CopyCompleteValue. NeedToReload instead tests
        // an empty clip and is only the game's automatic-reload predicate.
        if (loaded >= maximum) { known(Action::Reload, false); return; }
        bool spare = false;
        if (boolQuery(target, weapon, "HasSpareAmmo", "WillowWeapon", 0x00020102, spare) &&
            targetUnchanged(target)) known(Action::Reload, spare);
    }
    void known(Action action, bool enabled) {
        const auto bit = actionBit(action);
        observation.knownActions |= bit;
        if (!enabled) available &= ~bit;
    }
    bool triggerUseQuery(const Target& target,bool& usable) {
        usable=false;
        const auto fn=function(target.controller,"GetTriggerUseList","WillowPlayerController");
        const auto distance=field(target.controller,"InteractDistance","FloatProperty");
        const auto arrayParam=fn ? property(fn,"out_useList","ArrayProperty") : Property{};
        const auto only=fn ? property(fn,"bUsuableOnly","BoolProperty") : Property{};
        bool schema=fn && read(fn+0x7C)==0x00C20002 && host.MemoryRead8(fn+0x85)==5 &&
            host.MemoryRead16(fn+0x86)==28 && host.MemoryRead16(fn+0x88)==0xFFFF &&
            read(read(target.controller)+0x10C)==base+kActorProcessEvent &&
            distance && distance.size==4 && distance.dimension==1 &&
            only && only.offset==12 && only.size==4 && only.dimension==1 && only.extra==1 &&
            host.MemoryRead64(only.object+0x48)==0x80 &&
            arrayParam && arrayParam.offset==16 && arrayParam.size==12 && arrayParam.dimension==1 &&
            host.MemoryRead64(arrayParam.object+0x48)==0x00400180 &&
            token(arrayParam.extra) && className(arrayParam.extra)=="ObjectProperty" &&
            read(arrayParam.extra+0x28)==arrayParam.object &&
            read(arrayParam.extra+0x40)==1 && read(arrayParam.extra+0x44)==4 &&
            objectName(read(arrayParam.extra+0x74))=="Trigger";
        constexpr const char* names[]{"interactDistanceToCheck","crosshairDist","minDot"};
        for (unsigned i=0;schema && i<3;++i) {
            const auto p=property(fn,names[i],"FloatProperty");
            schema=p && p.offset==i*4 && p.size==4 && p.dimension==1 && host.MemoryRead64(p.object+0x48)==0x80;
        }
        const auto range=distance ? wordFloat(read(target.controller+distance.offset)) : 0;
        if (!schema || !std::isfinite(range) || range<=0 || range>1000000 || !targetUnchanged(target)) {
#if HORN_ENABLE_LOGGING
            useReason="use-trigger-schema";
            queryDiagnostic("GetTriggerUseList",fn,only,arrayParam);
#endif
            return false;
        }
        if (!queryScratch) queryScratch=host.GuestAlloc(32,8);
        if (!queryScratch) return false;
        // Engine.PlayerController.TriggerInteracted's exact original arguments.
        // The out TArray starts empty, so every returned Data allocation is
        // owned by this query and must be freed by the game's own allocator.
        const std::array<std::uint32_t,8> params{floatWord(range),floatWord(60),0,1,0,0,0,0};
        for (std::uint32_t i=0;i<params.size();++i) host.MemoryWrite32(queryScratch+i*4,params[i]);
        const auto identity=token(fn);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base+kActorProcessEvent,{target.controller,fn,queryScratch,0});
#else
        const bool called=Bl2TouchCallGuest(host,base+kActorProcessEvent,{target.controller,fn,queryScratch,0});
#endif
        std::uint32_t data=0,count=0;
#if defined(BL2_NATIVE_ARM32)
        bool valid=array(queryScratch+16,64,data,count);
#else
        bool valid=called && array(queryScratch+16,64,data,count);
#endif
        for (std::uint32_t i=0;valid && i<count;++i) valid=isA(read(data+i*4),"Trigger");
        // Cleanup is required even if the callback changed scene/lifetime or
        // produced an oversized list. Never use Host.GuestFree on UE3's Data.
        const auto allocation=read(queryScratch+16);
        if (allocation) {
            if (!span(allocation,4)) {
                triggerQueriesDisabled=true;valid=false;
            }
#if defined(BL2_NATIVE_ARM32)
            else host.RunGuestCallback(base+kAppFree,{allocation});
#else
            else if (!Bl2TouchCallGuest(host,base+kAppFree,{allocation})) {
                triggerQueriesDisabled=true;valid=false;
            }
#endif
        }
        host.MemoryWrite32(queryScratch+16,0);host.MemoryWrite32(queryScratch+20,0);host.MemoryWrite32(queryScratch+24,0);
#if HORN_ENABLE_LOGGING
        useReason=valid ? (count ? "use-trigger-present" : "use-trigger-empty") : "use-trigger-result";
#endif
        usable=count!=0;
        return valid && token(fn)==identity && targetUnchanged(target);
    }
    UseAvailability pickupUseQuery(const Target& target) {
        // Original PickupSomething chooses this IPickupable independently of
        // CurrentUsableObject. The return is an 8-byte script interface, not a
        // UObject pointer. No pickup/deny/throw command is run by this observer.
        const auto fn=function(target.controller,"GetCurrentPickupable","WillowPlayerController");
        const auto ret=fn ? property(fn,"ReturnValue","InterfaceProperty") : Property{};
        if (!fn || read(fn+0x7C)!=0x00020102 || host.MemoryRead8(fn+0x85)!=1 ||
            host.MemoryRead16(fn+0x86)!=8 || host.MemoryRead16(fn+0x88)!=0 ||
            !ret || ret.offset || ret.size!=8 || ret.dimension!=1 ||
            host.MemoryRead64(ret.object+0x48)!=0x580 || objectName(ret.extra)!="IPickupable" ||
            read(read(target.controller)+0x10C)!=base+kActorProcessEvent || !targetUnchanged(target)) {
#if HORN_ENABLE_LOGGING
            useReason="use-pickup-getter-schema";queryDiagnostic("GetCurrentPickupable",fn,ret,{});
#endif
            return {};
        }
        if (!queryScratch) queryScratch=host.GuestAlloc(32,8);
        if (!queryScratch) return {};
        const auto getterIdentity=token(fn);
        host.MemoryWrite32(queryScratch,0xFFFFFFFFu);host.MemoryWrite32(queryScratch+4,0xFFFFFFFFu);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base+kActorProcessEvent,{target.controller,fn,queryScratch,0});
#else
        if (!Bl2TouchCallGuest(host,base+kActorProcessEvent,{target.controller,fn,queryScratch,0})) return {};
#endif
        const auto object=read(queryScratch),iface=read(queryScratch+4);
        if (!targetUnchanged(target) || token(fn)!=getterIdentity) return {};
        if (!object && !iface) return {true,false};
        // The pinned IIPickupable adjustment thunk is object+392. Validate its
        // CostsToPickUp slot before passing the interface to the original
        // affordability query; unreviewed implementations stay unknown.
        if (!isA(object,"WillowPickup") || iface!=object+392 || !span(iface,4) ||
            !span(read(iface),44) || read(read(iface)+0x28)!=base+0x02277440) return {};
        const auto identity=token(object);
        bool enabled=false;
        if (!boolQuery(target,object,"Pickupable_IsEnabled","WillowPickup",0x00020400,enabled) ||
            token(object)!=identity) return {};
        if (!enabled) return {true,false};
        const auto afford=function(target.controller,"CanAffordToPickUpPickupable","WillowPlayerController");
        const auto arg=afford ? property(afford,"Pickupable","InterfaceProperty") : Property{};
        const auto result=afford ? property(afford,"ReturnValue","BoolProperty") : Property{};
        if (!afford || read(afford+0x7C)!=0x00020401 || host.MemoryRead8(afford+0x85)!=2 ||
            host.MemoryRead16(afford+0x86)!=12 || host.MemoryRead16(afford+0x88)!=8 ||
            !arg || arg.offset || arg.size!=8 || arg.dimension!=1 ||
            host.MemoryRead64(arg.object+0x48)!=0x80 || objectName(arg.extra)!="IPickupable" ||
            !result || result.offset!=8 || result.size!=4 || result.dimension!=1 || result.extra!=1 ||
            host.MemoryRead64(result.object+0x48)!=0x580 || !targetUnchanged(target)) {
#if HORN_ENABLE_LOGGING
            useReason="use-pickup-afford-schema";queryDiagnostic("CanAffordToPickUpPickupable",afford,arg,result);
#endif
            return {};
        }
        const auto affordIdentity=token(afford);
        host.MemoryWrite32(queryScratch,object);host.MemoryWrite32(queryScratch+4,iface);
        host.MemoryWrite32(queryScratch+8,0xFFFFFFFFu);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base+kActorProcessEvent,{target.controller,afford,queryScratch,0});
#else
        if (!Bl2TouchCallGuest(host,base+kActorProcessEvent,{target.controller,afford,queryScratch,0})) return {};
#endif
        const auto value=read(queryScratch+8);
        if (value>1 || token(object)!=identity || token(afford)!=affordIdentity || !targetUnchanged(target)) return {};
#if HORN_ENABLE_LOGGING
        useReason=value ? "use-pickup-present" : "use-pickup-unaffordable";
#endif
        // A full inventory can still swap the active weapon/equipment through
        // PickupSomething. DenyPickupAttempt may open a native movie, so neither
        // it nor inventory mutation is used as a passive availability predicate.
        return {true,value!=0};
    }
    void observeUse(const Target& target,std::uint32_t pawn) {
        const auto p=field(target.controller,"CurrentUsableObject","InterfaceProperty");
        if (!p || p.size!=8 || p.dimension!=1 || !span(target.controller+p.offset,8)) {
#if HORN_ENABLE_LOGGING
            useReason="use-interface-schema";
            queryDiagnostic("CurrentUsableObject",0,p,{});
#endif
            return;
        }
        const auto object=read(target.controller+p.offset),iface=read(target.controller+p.offset+4);
        if (object && token(object) && span(iface,4)) {
            known(Action::Use,true);
#if HORN_ENABLE_LOGGING
            useReason="use-current-object";
#endif
            return;
        }
        if (object || iface || triggerQueriesDisabled) {
#if HORN_ENABLE_LOGGING
            useReason=triggerQueriesDisabled ? "use-trigger-disabled" : "use-interface-pair";
#endif
            return;
        }
        const auto now=monotonicMilliseconds();
        if (!cachedAvailability(useCache,target,pawn,now)) {
            useCache={target,token(pawn),now,false,false};
            bool trigger=false;
            const UseAvailability normal{triggerUseQuery(target,trigger),trigger};
            const auto combined=Bl2CombineUseAvailability(normal,pickupUseQuery(target));
            useCache.known=combined.known;useCache.usable=combined.usable;
#if HORN_ENABLE_LOGGING
            useCache.reason=useReason;
#endif
        }
#if HORN_ENABLE_LOGGING
        useReason=useCache.reason;
#endif
        if (useCache.known && targetUnchanged(target)) known(Action::Use,useCache.usable);
    }
    bool cachedAvailability(const AvailabilityCache& cache,const Target& target,std::uint32_t pawn,std::uint64_t now) const {
        const auto& previous=cache.target;
        return cache.time && now-cache.time<100 && previous.controllerToken==target.controllerToken &&
            previous.inputToken==target.inputToken && previous.world==target.world && previous.scene==target.scene &&
            previous.generation==target.generation && previous.fingerprint==target.fingerprint && cache.pawn==token(pawn);
    }
    bool skillQuery(const Target& target,std::uint32_t pawn,std::uint32_t tree,bool& usable) {
        usable=false;
        const auto get=function(tree,"GetActionSkill","PlayerSkillTree");
        const auto definition=get ? property(get,"ReturnValue","ObjectProperty") : Property{};
        if (!get || read(get+0x7C)!=0x00020400 || host.MemoryRead8(get+0x85)!=1 ||
            host.MemoryRead16(get+0x86)!=4 || host.MemoryRead16(get+0x88)!=0 ||
            !definition || definition.offset!=0 || definition.size!=4 || definition.dimension!=1 ||
            host.MemoryRead64(definition.object+0x48)!=0x580 || objectName(definition.extra)!="SkillDefinition" ||
            !span(tree,0xA8) || !targetUnchanged(target)) {
#if HORN_ENABLE_LOGGING
            skillReason="skill-definition-schema";
            queryDiagnostic("GetActionSkill",get,definition,{});
#endif
            return false;
        }
        const auto treeIdentity=token(tree);
#if defined(BL2_NATIVE_ARM32)
        const auto skill=host.RunGuestCallback(base+kGetActionSkill,{tree});
#else
        std::uint32_t skill=0;
        if (!Bl2TouchCallGuest(host,base+kGetActionSkill,{tree},skill)) return false;
#endif
        if (!targetUnchanged(target) || token(tree)!=treeIdentity) return false;
        if (!skill) {
#if HORN_ENABLE_LOGGING
            skillReason="skill-no-definition";
#endif
            return true;
        }
        if (!isA(skill,"SkillDefinition")) return false;
        const auto gradeFn=function(target.controller,"GetSkillGrade","WillowPlayerController");
        const auto arg=gradeFn ? property(gradeFn,"Definition","ObjectProperty") : Property{};
        const auto ret=gradeFn ? property(gradeFn,"ReturnValue","IntProperty") : Property{};
        if (!gradeFn || read(gradeFn+0x7C)!=0x00820002 || host.MemoryRead8(gradeFn+0x85)!=2 ||
            host.MemoryRead16(gradeFn+0x86)!=8 || host.MemoryRead16(gradeFn+0x88)!=4 ||
            read(read(target.controller)+0x10C)!=base+kActorProcessEvent ||
            !arg || arg.offset!=0 || arg.size!=4 || arg.dimension!=1 || objectName(arg.extra)!="SkillDefinition" ||
            host.MemoryRead64(arg.object+0x48)!=0x80 || !ret || ret.offset!=4 || ret.size!=4 || ret.dimension!=1 ||
            host.MemoryRead64(ret.object+0x48)!=0x580) {
#if HORN_ENABLE_LOGGING
            skillReason="skill-grade-schema";
            queryDiagnostic("GetSkillGrade",gradeFn,arg,ret);
#endif
            return false;
        }
        if (!queryScratch) queryScratch=host.GuestAlloc(32,8);
        if (!queryScratch) return false;
        const auto skillIdentity=token(skill),fnIdentity=token(gradeFn);
        host.MemoryWrite32(queryScratch,skill);host.MemoryWrite32(queryScratch+4,0x80000000u);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base+kActorProcessEvent,{target.controller,gradeFn,queryScratch,0});
#else
        if (!Bl2TouchCallGuest(host,base+kActorProcessEvent,{target.controller,gradeFn,queryScratch,0})) return false;
#endif
        const auto grade=static_cast<std::int32_t>(read(queryScratch+4));
        if (!targetUnchanged(target) || token(skill)!=skillIdentity || token(gradeFn)!=fnIdentity || grade< -1 || grade>65535)
            return false;
        if (grade<=0) {
#if HORN_ENABLE_LOGGING
            skillReason="skill-unlearned";
#endif
            return true;
        }
        bool cooldown=false,running=false;
        if (!boolQuery(target,target.controller,"IsActionSkillOnCooldown","WillowPlayerController",0x00020002,cooldown) ||
            (cooldown && !boolQuery(target,pawn,"IsActionSkillRunning","WillowPawn",0x00020400,running))) {
#if HORN_ENABLE_LOGGING
            skillReason="skill-cooldown-schema";
#endif
            return false;
        }
        // Learned skills can have an active native toggle while the cooldown
        // pool is nonzero. Preserve that state; only a cooling, inactive skill
        // is unavailable. Original input availability remains an extra gate.
        usable=!cooldown || running;
#if HORN_ENABLE_LOGGING
        skillReason=running ? "skill-running" : (cooldown ? "skill-cooldown" : "skill-ready");
#endif
        return true;
    }
    void observeSkill(const Target& target,std::uint32_t pawn) {
        std::uint32_t tree=0;
        if (!objectField(target.controller,"PlayerSkillTree",tree)) return;
        if (!tree) {
            known(Action::Skill,false);
#if HORN_ENABLE_LOGGING
            skillReason="skill-no-tree";
#endif
            return;
        }
        if (!isA(tree,"PlayerSkillTree")) return;
        const auto now=monotonicMilliseconds();
        if (skillTreeToken!=token(tree) || !cachedAvailability(skillCache,target,pawn,now)) {
            skillTreeToken=token(tree);skillCache={target,token(pawn),now,false,false};
            skillCache.known=skillQuery(target,pawn,tree,skillCache.usable);
#if HORN_ENABLE_LOGGING
            skillCache.reason=skillReason;
#endif
        }
#if HORN_ENABLE_LOGGING
        skillReason=skillCache.reason;
#endif
        if (skillCache.known && targetUnchanged(target)) known(Action::Skill,skillCache.usable);
    }
    bool readiedInventory(std::uint32_t manager, std::vector<std::uint32_t>& weapons) const {
        if (!isA(manager, "WillowInventoryManager")) return false;
        const auto chain = field(manager, "InventoryChain", "ObjectProperty");
        // CountReadiedWeapons reads its chain and every native Inventory link
        // at +188, and counts WillowWeapon.bReadied at +19C bit1.
        if (!chain || chain.size != 4 || chain.offset != 0x188 || chain.dimension != 1) return false;
        auto item = read(manager + chain.offset);
        std::vector<std::uint32_t> seen;
        for (unsigned i = 0; item && i != 256; ++i) {
            if (!isA(item, "Inventory") || !span(item, 0x1A0) ||
                std::find(seen.begin(), seen.end(), item) != seen.end()) return false;
            seen.push_back(item);
            if (isA(item, "WillowWeapon") && (host.MemoryRead8(item + 0x19C) & 2u)) {
                weapons.push_back(item);
                if (weapons.size() > 64) return false;
            }
            item = read(item + 0x188);
        }
        return !item;
    }
    bool lastWeaponSlot(const Target& target, std::uint8_t& slot, bool* knownState = nullptr) const {
        if (knownState) *knownState = false;
        std::uint32_t pawn = 0, manager = 0, last = 0, currentWeapon = 0;
        if (!targetUnchanged(target) || !objectField(target.controller, "Pawn", pawn) ||
            !isA(pawn, "WillowPawn") || !objectField(pawn, "InvManager", manager) ||
            !objectField(pawn, "Weapon", currentWeapon)) return false;
        const auto previous = weaponHistory.candidate(target.controllerToken, token(pawn), target.world, token(currentWeapon));
        last = static_cast<std::uint32_t>(previous >> 32);
        if (previous && token(last) != previous) last = 0;
        std::vector<std::uint32_t> weapons;
        if (!readiedInventory(manager, weapons)) return false;
        if (weapons.size() < 2 || !last || last == currentWeapon ||
            std::find(weapons.begin(), weapons.end(), last) == weapons.end()) {
            if (knownState) *knownState = true;
            return false;
        }
        const auto quick = field(last, "QuickSelectSlot", "ByteProperty");
        if (!quick || quick.size != 1 || quick.dimension != 1 || !span(last + quick.offset, 1)) return false;
        slot = host.MemoryRead8(last + quick.offset);
        if (slot < 1 || slot > 4) return false;
        // The original command addresses a slot, not an inventory pointer.
        // Reject duplicate slot assignments instead of selecting another item.
        for (const auto item : weapons) {
            if (item == last) continue;
            const auto other = field(item, "QuickSelectSlot", "ByteProperty");
            if (!other || other.size != 1 || other.dimension != 1 || !span(item + other.offset,1) ||
                host.MemoryRead8(item + other.offset) == slot) return false;
        }
        if (knownState) *knownState = true;
        return true;
    }
    std::uint32_t lastWeaponCommand(std::uint32_t controller) const {
        const auto fn = function(controller, "EquipWeaponFromSlot", "WillowPlayerController");
        const auto arg = fn ? property(fn, "QuickSlot", "ByteProperty") : Property{};
        return fn && arg && read(read(controller) + 0x10C) == base + kActorProcessEvent &&
            arg.offset == 0 && arg.size == 1 && arg.dimension == 1 &&
            read(fn + 0x7C) == 0x00020202 && host.MemoryRead8(fn + 0x85) == 1 &&
            host.MemoryRead16(fn + 0x86) == 1 && host.MemoryRead16(fn + 0x88) == 0xFFFF &&
            (host.MemoryRead64(arg.object + 0x48) & 0x580u) == 0x80u ? fn : 0;
    }
    bool selectLast(const Target& target) {
        std::uint8_t slot = 0;
        const auto fn = lastWeaponCommand(target.controller);
        if (!fn || !lastWeaponSlot(target, slot)) return false;
        if (!queryScratch) queryScratch = host.GuestAlloc(32, 8);
        if (!queryScratch || !current()) return false;
        host.MemoryWrite32(queryScratch, slot);
        // The game's normal controller command owns restrictions, switching,
        // animation and networking. We write only
        // our call parameters, never an equipped item or inventory slot.
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base + kActorProcessEvent, {target.controller, fn, queryScratch, 0});
        return true;
#else
        return Bl2TouchCallGuest(host, base + kActorProcessEvent, {target.controller, fn, queryScratch, 0});
#endif
    }
    const char* hudRejectReason() const {
        const auto object = static_cast<std::uint32_t>(hudSource.owner >> 32);
        const auto gfx = read(base + 0x0301F840);
        if (!hudSource.owner || token(object)!=hudSource.owner || !span(object,0x264)) return "hud-owner";
        if (!targetUnchanged(hudSource.target)) return "input-lifetime";
        if (read(object+0x260)!=hudSource.target.controller) return "hud-controller";
        if (!span(hudSource.movie,0x6C) || read(object+0x3C)!=hudSource.movie ||
            read(hudSource.movie+0x64)!=object) return "movie-owner";
        if (!span(hudSource.interface,4) || read(hudSource.movie+0x34)!=hudSource.interface) return "movie-interface";
        // StartScene sets +48, UGFxMoviePlayer.Start tests it, and CloseScene
        // clears it. +5C is SetMovieCanReceiveInput, not passive HUD liveness.
        // The existing original HUD layout guard also rejects render textures.
        if (!read(hudSource.movie+0x48)) return "movie-not-started";
        if (read(hudSource.movie+0x68)) return "movie-render-target";
        if (read(object+0xCC)) return "hud-render-target";
        if (!span(hudSource.viewport,0x44)) return "viewport-native";
        if (!span(read(hudSource.viewport+0x20),4)) return "viewport-client";
        if (!span(gfx,0x48) || read(gfx+0x44)!=hudSource.viewport+4) return "gfx-viewport";
        if (!hudSource.width || !hudSource.height || hudSource.width>16384 || hudSource.height>16384 ||
            read(hudSource.viewport+0x3C)!=hudSource.width || read(hudSource.viewport+0x40)!=hudSource.height)
            return "viewport-size";
        return nullptr;
    }
    bool hudCurrent() const { return hudRejectReason()==nullptr; }
    static bool visibleClip(GfxSession& session, GfxHandle clip) {
        const auto v=session.member(clip,"_visible"),a=session.member(clip,"_alpha");
        bool visible=false;double alpha=0;
        const bool okay=v && a && session.boolean(v,visible) && visible &&
            session.number(a,alpha) && alpha>0 && alpha<=100;
        session.release(v);session.release(a);return okay;
    }
    bool counterBounds(GfxSession& session,GfxHandle bullets,GfxRect& out) {
        const auto counter=session.member(bullets,"bullet_c");
        const auto parent=session.member(counter,"_parent");
        GfxIdentity expected,actual;
        bool okay=counter && visibleClip(session,counter) && session.identity(bullets,expected) &&
            session.identity(parent,actual) && expected==actual;
        std::array<double,7> v{};
        constexpr const char* names[]{"_x","_y","_width","_height","_xscale","_yscale","_rotation"};
        for (std::size_t i=0;okay && i!=v.size();++i) {
            const auto value=session.member(counter,names[i]);
            okay=value && session.number(value,v[i]);session.release(value);
        }
#if HORN_ENABLE_LOGGING
        ammoMetrics=v;
#endif
        session.release(parent);session.release(counter);
        // Original HUD EditText671/955 has a fixed, non-autosized own rectangle
        // [1573,5347,-40,731] twips. Its nonzero origin must be retained: _x/_y
        // alone are not the top-left of the displayed number. Parent bullets
        // carries the inherited HUD3D transform and interface scaling.
        std::array<double,4> rect{};
        if (!okay || !Bl2AmmoCounterLocalRect(v,rect)) return false;
        out={rect[0],rect[1],rect[2],rect[3]};
        return true;
    }
    static bool surfaceRegion(const GfxRect& stage,const GfxRect& frame,const GfxViewport& viewport,
                              std::uint32_t width,std::uint32_t height,SurfaceRect& rect) {
        WidgetPoint a,b;
        if (!NormalizeWidgetPoint(frame,viewport,width,height,{stage.left,stage.top},a) ||
            !NormalizeWidgetPoint(frame,viewport,width,height,{stage.right,stage.bottom},b)) return false;
        const double l=std::max(0.0,a.x),t=std::max(0.0,a.y),r=std::min(1.0,b.x),bottom=std::min(1.0,b.y);
        if (!std::isfinite(l) || !std::isfinite(t) || !std::isfinite(r) || !std::isfinite(bottom) || r<=l || bottom<=t)
            return false;
        rect={static_cast<float>(l),static_cast<float>(t),static_cast<float>(r),static_cast<float>(bottom)};
        return true;
    }
    void hintDiagnostic(unsigned index,const UseHintObservation& hint,const char* vmReason,
                        const GfxRect& frame,const GfxViewport& viewport,const SurfaceRect& rect,bool projected) {
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if (index>=lastHintDiagnostic.size() || hintDiagnosticLogs>=16) return;
        char property[PROP_VALUE_MAX]{};
        __system_property_get("debug.bl2bridge.touch_diagnostics",property);
        if (property[0]!='1' || property[1]) return;
        const char* reason=vmReason ? vmReason : (!projected && std::strcmp(hint.reason,"projected")==0 ? "surface-region" : hint.reason);
        auto& previous=lastHintDiagnostic[index];
        if (previous.owner==hudSource.owner && previous.reason==reason && previous.frame==hint.frame &&
            std::abs(previous.rect.left-rect.left)<=.02f && std::abs(previous.rect.top-rect.top)<=.02f &&
            std::abs(previous.rect.right-rect.right)<=.02f && std::abs(previous.rect.bottom-rect.bottom)<=.02f) return;
        previous={hudSource.owner,reason,hint.frame,rect};++hintDiagnosticLogs;
        HORN_LOGI("BL2TouchDiagnostics","native-use hint=%s owner=%08X projected=%u reason=%s frame=%.0f/%.0f types=%u/%u "
            "parent=%08X/%08X asDepth=%.0f metrics=(%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f) "
            "local=(%.4f,%.4f,%.4f,%.4f) stage=(%.4f,%.4f,%.4f,%.4f) "
            "surface=(%.6f,%.6f,%.6f,%.6f) size=%ux%u frameRect=(%.4f,%.4f,%.4f,%.4f) viewport=(%u,%u,%u,%u,%u,%u)",
            index ? "toolsSecondary" : "tools",static_cast<std::uint32_t>(hudSource.owner>>32),projected ? 1u : 0u,
            reason,hint.frame,hint.currentFrame,hint.toolsType,hint.descType,hint.tools.data,hint.parent.data,hint.depth,
            hint.metrics[0],hint.metrics[1],hint.metrics[2],hint.metrics[3],hint.metrics[4],hint.metrics[5],hint.metrics[6],
            hint.local.left,hint.local.top,hint.local.right,hint.local.bottom,
            hint.stage.left,hint.stage.top,hint.stage.right,hint.stage.bottom,rect.left,rect.top,rect.right,rect.bottom,
            hudSource.width,hudSource.height,frame.left,frame.top,frame.right,frame.bottom,
            viewport.words[0],viewport.words[1],viewport.words[2],viewport.words[3],viewport.words[4],viewport.words[5]);
#else
        (void)index;(void)hint;(void)vmReason;(void)frame;(void)viewport;(void)rect;(void)projected;
#endif
    }
    void observeHud(const Target& target, std::uint32_t hud,bool armed,bool usable) {
        HudSource next; next.target=target;next.owner=token(hud);next.armed=armed;next.usable=usable;
        next.movie=read(hud + 0x3C);next.interface=read(next.movie + 0x34);
        // FViewport is a native object, not a UObject/LocalPlayer property.
        // The existing input-boundary hook supplies this exact live viewport.
        next.viewport=target.viewport;
        next.width=read(next.viewport + 0x3C);next.height=read(next.viewport + 0x40);
        const bool same=hudSource.owner==next.owner && hudSource.movie==next.movie &&
            hudSource.interface==next.interface && hudSource.viewport==next.viewport &&
            hudSource.width==next.width && hudSource.height==next.height &&
            hudSource.armed==next.armed && hudSource.usable==next.usable &&
            hudSource.target.scene==target.scene && hudSource.target.generation==target.generation;
        hudSource=next;
        if (!same) {hudObservation={};hudSampleTime=0;}
        if (const auto reason=hudRejectReason()) {
#if HORN_ENABLE_LOGGING
            hudReason=reason;objectiveReason="objective-native-source";
#else
            (void)reason;
#endif
            hudObservation={};hudSampleTime=0;return;
        }
        const auto now=monotonicMilliseconds();
        if (!hudSampleTime || now-hudSampleTime>=100) {
            hudSampleTime=now;hudObservation={};
            if (!hudTransport) hudTransport=std::make_unique<GfxHostTransport>(host,[this]{return hudCurrent();});
            GfxSession session(*hudTransport,base,next.interface,hudTransport->scratch(),256);
            const auto root=session.variable("_root"),p1=session.member(root,"p1"),
                echo=session.member(p1,"minimapEcho"),minimap=session.member(echo,"minimap"),
                mask=session.member(minimap,"mapmask"),ammo=session.member(p1,"bullets");
            GfxRect frame;GfxViewport viewport;
            if (session.frameRect(frame) && session.viewport(viewport) && visibleClip(session,root) && visibleClip(session,p1)) {
                // The persistent map mask bounds exactly the rendered map.
                // Mask clips themselves are intentionally invisible after
                // setMask; their visible ancestors own visibility. Full Echo
                // bounds also contain off-map prototypes and transient effects.
                GfxRect mapStage,ammoLocal,ammoStage;
                if (mask && visibleClip(session,echo) && visibleClip(session,minimap) &&
                    session.projectedBounds(mask,mapStage) && surfaceRegion(mapStage,frame,viewport,next.width,next.height,hudObservation.mapRect))
                    hudObservation.hudValidMask|=HudMinimap;
                // Native UpdateAmmo writes p1.bullets.bullet_c.text and the
                // bullet_icon timeline; `ammo` is not this movie's clip name.
                if (armed && ammo && visibleClip(session,ammo) && counterBounds(session,ammo,ammoLocal) &&
                    session.projectedRect(ammo,ammoLocal,ammoStage) && surfaceRegion(ammoStage,frame,viewport,next.width,next.height,hudObservation.ammoRect))
                    hudObservation.hudValidMask|=HudAmmo;
#if HORN_ENABLE_LOGGING
                hudReason=session.reason() ? session.reason() : (hudObservation.hudValidMask & HudAmmo ? "observed" :
                    (armed ? "ammo-counter" : "unarmed"));
#endif
            } else {
#if HORN_ENABLE_LOGGING
                hudReason=session.reason() ? session.reason() : "frame-root-visibility";
#endif
            }
            session.release(ammo);session.release(mask);session.release(minimap);
            session.release(echo);session.release(p1);session.release(root);session.close();
            if (!session.cleanupOkay() || !hudCurrent()) hudObservation={};
            // Separate call budget and managed-value lifetime: a missing or
            // malformed quest row must not discard good map/ammo observations.
            GfxSession objectives(*hudTransport,base,next.interface,hudTransport->scratch(),512);
            const auto objectiveRoot=objectives.variable("_root"),objectiveP1=objectives.member(objectiveRoot,"p1"),
                mission=objectives.member(objectiveP1,"MissionNotifications");
            GfxRect objectiveFrame;GfxViewport objectiveViewport;
            HudObjectiveBounds bounds;
            if (objectives.frameRect(objectiveFrame) && objectives.viewport(objectiveViewport) &&
                visibleClip(objectives,objectiveRoot) && visibleClip(objectives,objectiveP1)) {
                bounds=ObserveBl2HudObjectives(objectives,mission);
                if (bounds.state==HudObjectiveState::Empty) hudObservation.hudValidMask|=HudObjectivesEmpty;
                else if (bounds.state==HudObjectiveState::Projected &&
                    surfaceRegion(bounds.stage,objectiveFrame,objectiveViewport,next.width,next.height,hudObservation.objectiveRect))
                    hudObservation.hudValidMask|=HudObjectives;
            }
#if HORN_ENABLE_LOGGING
            objectiveReason=objectives.reason() ? objectives.reason() : bounds.reason;
            if (bounds.state==HudObjectiveState::Projected && !(hudObservation.hudValidMask & HudObjectives))
                objectiveReason="objective-surface-projection";
#endif
            objectives.release(mission);objectives.release(objectiveP1);objectives.release(objectiveRoot);objectives.close();
            if (!objectives.cleanupOkay() || !hudCurrent()) hudObservation={};
            if (usable && hudCurrent()) {
                // A distinct budget/lifetime keeps an absent tooltip from
                // discarding the genuine map/ammo/mission regions. `desc` is
                // the original ShowToolTip caption, not contextualPrompt or
                // whole tools bounds containing other timeline artwork.
                GfxSession hint(*hudTransport,base,next.interface,hudTransport->scratch(),192);
                const auto hintRoot=hint.variable("_root"),hintP1=hint.member(hintRoot,"p1");
                GfxRect hintFrame,stage;GfxViewport hintViewport;
                if (hint.frameRect(hintFrame) && hint.viewport(hintViewport) &&
                    visibleClip(hint,hintRoot) && visibleClip(hint,hintP1)) {
                    // Primary interaction/pickup caption wins. The original
                    // secondary caption is used only when primary is absent;
                    // do not make a transparent union across empty HUD space.
                    unsigned hintIndex=0;
                    for (const auto name:{"tools","toolsSecondary"}) {
                        const auto tools=hint.member(hintP1,name);
                        UseHintObservation detail;
                        const bool projected=ObserveBl2UseHint(hint,tools,stage,&detail) &&
                            surfaceRegion(stage,hintFrame,hintViewport,next.width,next.height,hudObservation.useRect);
                        hintDiagnostic(hintIndex++,detail,hint.reason(),hintFrame,hintViewport,hudObservation.useRect,projected);
                        hint.release(tools);
                        if (projected) {hudObservation.hudValidMask|=HudUse;break;}
                        if (!hint.good()) break;
                    }
                }
                hint.release(hintP1);hint.release(hintRoot);hint.close();
                if (!hint.cleanupOkay() || !hudCurrent()) hudObservation={};
            }
        }
        observation.hudValidMask=hudObservation.hudValidMask;
        observation.mapRect=hudObservation.mapRect;observation.ammoRect=hudObservation.ammoRect;
        observation.objectiveRect=hudObservation.objectiveRect;
        observation.useRect=hudObservation.useRect;
    }
    void diagnostic() {
#if HORN_ENABLE_LOGGING
        const auto now=monotonicMilliseconds();
        const auto rectChanged=[](const SurfaceRect& a,const SurfaceRect& b) {
            return std::abs(a.left-b.left)>.002f || std::abs(a.top-b.top)>.002f ||
                std::abs(a.right-b.right)>.002f || std::abs(a.bottom-b.bottom)>.002f;
        };
        const auto changed=lastDiagnosticOwner!=hudSource.owner || lastDiagnostic.weaponCount!=observation.weaponCount || lastDiagnostic.grenadeCount!=observation.grenadeCount ||
            lastDiagnostic.knownActions!=observation.knownActions || lastDiagnostic.availableActions!=available ||
            lastDiagnostic.hudValidMask!=observation.hudValidMask ||
            rectChanged(lastDiagnostic.mapRect,observation.mapRect) || rectChanged(lastDiagnostic.ammoRect,observation.ammoRect) ||
            rectChanged(lastDiagnostic.objectiveRect,observation.objectiveRect) || rectChanged(lastDiagnostic.useRect,observation.useRect);
        if (diagnosticLogs>=32 || (!changed && now-diagnosticTime<5000)) return;
        ++diagnosticLogs;diagnosticTime=now;lastDiagnosticOwner=hudSource.owner;
        lastDiagnostic=observation;lastDiagnostic.availableActions=available;
        HORN_LOGI("BL2TouchDiagnostics","gameplay telemetry owner=%08X known=%llX available=%llX weapons=%d grenades=%d hud=%u reason=%s "
            "map=(%.6f,%.6f,%.6f,%.6f) ammo=(%.6f,%.6f,%.6f,%.6f) objectives=(%.6f,%.6f,%.6f,%.6f) hint=(%.6f,%.6f,%.6f,%.6f) objectiveReason=%s useReason=%s skillReason=%s",
            static_cast<std::uint32_t>(hudSource.owner>>32),
            static_cast<unsigned long long>(observation.knownActions),static_cast<unsigned long long>(available),
            observation.weaponCount,observation.grenadeCount,observation.hudValidMask,hudReason,
            observation.mapRect.left,observation.mapRect.top,observation.mapRect.right,observation.mapRect.bottom,
            observation.ammoRect.left,observation.ammoRect.top,observation.ammoRect.right,observation.ammoRect.bottom,
            observation.objectiveRect.left,observation.objectiveRect.top,observation.objectiveRect.right,observation.objectiveRect.bottom,
            observation.useRect.left,observation.useRect.top,observation.useRect.right,observation.useRect.bottom,
            objectiveReason,useReason,skillReason);
        if (!(observation.hudValidMask & HudAmmo)) HORN_LOGI("BL2TouchDiagnostics",
            "ammo counter metrics x=%.4f y=%.4f width=%.4f height=%.4f scale=(%.4f,%.4f) rotation=%.4f",
            ammoMetrics[0],ammoMetrics[1],ammoMetrics[2],ammoMetrics[3],ammoMetrics[4],ammoMetrics[5],ammoMetrics[6]);
#endif
    }
    void observeGameplay(const Target& target) {
        observation = {};
        std::uint32_t pawn = 0, manager = 0, weapon = 0;
        if (!isA(target.controller, "WillowPlayerController") ||
            !objectField(target.controller, "Pawn", pawn) || !isA(pawn, "WillowPawn")) return;
        if (objectField(pawn, "Weapon", weapon) && (!weapon || isA(weapon, "WillowWeapon")))
            weaponHistory.observe(target.controllerToken, token(pawn), target.world, token(weapon));
        if (objectField(pawn, "InvManager", manager)) {
            std::vector<std::uint32_t> weapons;
            if (readiedInventory(manager, weapons) && targetUnchanged(target)) {
#if defined(BL2_NATIVE_ARM32)
                const auto count = host.RunGuestCallback(base + kCountReadied, {manager});
                if (count == weapons.size() && count <= 64 && targetUnchanged(target)) {
#else
                std::uint32_t count = 0;
                if (Bl2TouchCallGuest(host, base + kCountReadied, {manager}, count) &&
                    count == weapons.size() && count <= 64 && targetUnchanged(target)) {
#endif
                    observation.weaponCount = static_cast<std::int32_t>(count);
                    known(Action::NextWeapon, count >= 2);
                    known(Action::PreviousWeapon, count >= 2);
                    if (lastWeaponCommand(target.controller)) {
                        std::uint8_t slot = 0;
                        bool lastKnown = false;
                        const bool canLast = lastWeaponSlot(target, slot, &lastKnown);
                        if (lastKnown) known(Action::LastWeapon, canLast);
                        if (canLast && (available & (actionBit(Action::NextWeapon) | actionBit(Action::PreviousWeapon))))
                            available |= actionBit(Action::LastWeapon);
                    }
                }
            }
        }
        const bool weaponKnown=objectField(pawn,"Weapon",weapon);
        if (weaponKnown && !weapon) known(Action::Reload,false);
        if (weaponKnown && !weapon && observation.weaponCount==0) {
            known(Action::Fire,false);known(Action::Aim,false);
        }
        if (weaponKnown && isA(weapon, "WillowWeapon")) {
            observeReload(target, weapon);
        }
        observeUse(target,pawn);
        observeSkill(target,pawn);
        if (!targetUnchanged(target)) return;
#if defined(BL2_NATIVE_ARM32)
        const auto hud = host.RunGuestCallback(base + kGetHudMovie, {target.controller});
        if (isA(hud, "WillowHUDGFxMovie") && span(hud, 0x440) &&
#else
        std::uint32_t hud = 0;
        if (Bl2TouchCallGuest(host, base + kGetHudMovie, {target.controller}, hud) &&
            isA(hud, "WillowHUDGFxMovie") && span(hud, 0x440) &&
#endif
            read(hud + 0x260) == target.controller && targetUnchanged(target)) {
            // Exact UpdateGrenades (02285780..B0) reads this UResourcePool and
            // converts the original GetCurrentValue result to an integer.
            const auto pool = read(hud + 0x43C);
            if (isA(pool, "ResourcePool") && span(pool, 0x70)) {
                const auto value = wordFloat(read(pool + 0x6C));
                if (std::isfinite(value) && value >= 0 && value <= 65535) {
                    observation.grenadeCount = static_cast<std::int32_t>(value);
                    known(Action::Grenade, observation.grenadeCount != 0);
                }
            }
            // A stale/off-frame ammo prototype is not an armed HUD region.
            // This also retires cached bounds immediately when holstered or
            // unarmed, rather than waiting for the next 100ms geometry sample.
            observeHud(target,hud,observation.weaponCount>0 && isA(weapon,"WillowWeapon"),
                (observation.knownActions & available & actionBit(Action::Use))!=0);
        } else {
            hudSource={};hudObservation={};hudSampleTime=0;
        }
        diagnostic();
    }
    bool localOwner(const Target& target) const {
        if (token(target.controller) != target.controllerToken || token(target.input) != target.inputToken ||
            !target.inputToken || !target.controllerToken || !isWillowInput(target.input) ||
            read(target.input + 0x28) != target.controller ||
            read(target.controller + 0x76C) != target.input ||
            read(base + 0x0300D940) != target.world) return false;
        const auto engine = read(base + 0x03008D20);
        std::uint32_t players = 0, count = 0;
        if (!token(engine) || !array(engine + 0x48C, 4, players, count) || count != 1) return false;
        const auto local = read(players);
        return token(local) && read(local + 0x40) == target.controller;
    }
    bool fingerprint(std::uint32_t input, std::uint64_t& result) const {
        result = 1;
        std::uint32_t contexts = 0, count = 0;
        if (!array(input + 0x1A4, 32, contexts, count) || !count) return false;
        result = mix(result, contexts); result = mix(result, count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto ctx = read(contexts + i * 4);
            if (ctx && !token(ctx)) return false;
            result = mix(result, token(ctx));
        }
        const auto gfx = read(base + 0x0301F840);
        if (!gfx) return true;
        std::uint32_t focus = 0, focusedCount = 0;
        if (!span(gfx, 0x34) || !array(gfx + 0x28, 4, focus, focusedCount)) return false;
        result = mix(result, gfx); result = mix(result, focusedCount);
        if (focusedCount) {
            const auto movie = read(focus);
            if (movie) {
                if (!span(movie, 0x68) || !token(read(movie + 0x64))) return false;
                result = mix(result, movie); result = mix(result, token(read(movie + 0x64)));
            }
        }
        return true;
    }
    bool targetUnchanged(const Target& target) const {
        if (!generation || generation() != target.generation || !localOwner(target)) return false;
        std::uint64_t currentFingerprint = 0;
        return fingerprint(target.input, currentFingerprint) && currentFingerprint == target.fingerprint;
    }
    bool current() const {
        if (!frameValid || physicalOwnership().ownsInput(monotonicMilliseconds()) ||
            !targetUnchanged(frameTarget)) return false;
        const auto current = sharedMailbox().snapshot();
        return current.epoch == frameEpoch && current.context.sceneIdentity == frameTarget.scene &&
            (current.context.capabilities & GameplayInput) && !(current.context.capabilities & PhysicalOwnsInput);
    }
    bool collect(std::uint32_t definition, std::vector<std::uint32_t>& actions) const {
        std::uint32_t data = 0, count = 0;
        if (!token(definition) || !array(definition + 0x3C, 256, data, count)) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto action = read(data + i * 4);
            if (!action) continue;
            if (!token(action) || !span(action, 0x60)) return false;
            actions.push_back(action);
            if (actions.size() > 4096) return false;
        }
        return true;
    }
    bool actionDefinitions(std::uint32_t input, std::vector<std::uint32_t>& actions) const {
        std::uint32_t contexts = 0, count = 0;
        if (!array(input + 0x1A4, 32, contexts, count)) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto ctx = read(contexts + i * 4);
            if (!ctx) continue;
            if (!collect(ctx, actions)) return false;
            std::uint32_t sets = 0, setCount = 0;
            if (!array(ctx + 0x48, 64, sets, setCount)) return false;
            for (std::uint32_t j = 0; j < setCount; ++j) {
                const auto set = read(sets + j * 4);
                if (set && !collect(set, actions)) return false;
            }
        }
        std::sort(actions.begin(), actions.end());
        actions.erase(std::unique(actions.begin(), actions.end()), actions.end());
        return actions.size() <= 256;
    }
    bool availableNow(const Target& target, const Definition& definition) {
        if (!targetUnchanged(target) || token(definition.object) != definition.token) return false;
        // Native FireInputActionBehaviors uses this vtable entry for ordinary
        // actions. Refuse an unreviewed subclass override rather than bypass it.
        const auto vtable = read(target.input);
        if (read(vtable + 0x18C) != base + kAvailability) return false;
#if defined(BL2_NATIVE_ARM32)
        return definition.always || host.RunGuestCallback(base + kAvailability, {target.input, definition.object}) != 0;
#else
        std::uint32_t result = 0;
        return definition.always ||
            (Bl2TouchCallGuest(host, base + kAvailability, {target.input, definition.object}, result) && result != 0);
#endif
    }
    bool resolve(const Target& target, Action action, Definition& out) {
        const char* wanted = shippedActionName(action);
        if (!wanted || !targetUnchanged(target)) return false;
        std::vector<std::uint32_t> actions;
        if (!actionDefinitions(target.input, actions)) return false;
        unsigned matches = 0;
        for (const auto object : actions) {
            if (name(object + 0x58) != wanted) continue;
            out = {object, token(object), (host.MemoryRead8(object + 0x54) & 1u) != 0};
            ++matches;
        }
        return matches == 1 && availableNow(target, out);
    }
    bool invoke(const Target& target, const Definition& definition, bool down) {
        if (!arrayScratch || !localOwner(target) || token(definition.object) != definition.token) return false;
        host.MemoryWrite32(arrayScratch, arrayScratch + 12);
        host.MemoryWrite32(arrayScratch + 4, 1);
        host.MemoryWrite32(arrayScratch + 8, 1);
        host.MemoryWrite32(arrayScratch + 12, definition.object);
        // Flags proved by 0x236D924 and 0x236D950..99C:
        // (0,0) invokes only ordinary available actions; (0,1) invokes the
        // bAlwaysAvailable=1 partition. End with (1,bAlwaysAvailable) releases
        // only our recorded action even after its availability is revoked.
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base + kFireBehaviors, {
            target.input, arrayScratch, down ? 0u : 1u, down ? 0u : 1u,
            definition.always ? 1u : 0u});
        return true;
#else
        return Bl2TouchCallGuest(host, base + kFireBehaviors, {
            target.input, arrayScratch, down ? 0u : 1u, down ? 0u : 1u,
            definition.always ? 1u : 0u});
#endif
    }
    void release(std::size_t index) {
        const auto record = owned[index];
        owned[index] = {}; // Clear before callbacks: nested scene reset is safe.
        if (record.held) (void)invoke(record.target, record.definition, false);
    }
    void releaseAll() {
        for (std::size_t i = 0; i != owned.size(); ++i) release(i);
        movementPending = lookPending = false;
        right = forward = yaw = pitch = 0;
    }
    bool axis(std::uint32_t parameter, std::uint32_t expectedOffset, float delta) {
        if (!current() || !std::isfinite(delta)) return false;
        if (delta == 0) return true;
#if defined(BL2_NATIVE_ARM32)
        const auto pointer = host.RunGuestCallback(base + kAxisPointer, {frameTarget.input, parameter});
#else
        std::uint32_t pointer = 0;
        if (!Bl2TouchCallGuest(host, base + kAxisPointer, {frameTarget.input, parameter}, pointer)) return false;
#endif
        if (pointer != frameTarget.input + expectedOffset || !span(pointer, 4) || !current()) return false;
        const float value = wordFloat(read(pointer));
        if (!std::isfinite(value) || std::abs(value) > 1.0e7f) return false;
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base + kAddAxis, {frameTarget.input, pointer, floatWord(delta)});
        return true;
#else
        return Bl2TouchCallGuest(host, base + kAddAxis, {frameTarget.input, pointer, floatWord(delta)});
#endif
    }
};

Ue3GameplaySink::Ue3GameplaySink(DynarmicHost& host, LifetimeGeneration generation)
    : state_(std::make_unique<State>(host, generation)) {}
Ue3GameplaySink::~Ue3GameplaySink() = default;

std::uint64_t Ue3GameplaySink::prepare(std::uint32_t controller, std::uint32_t input, std::uint64_t scene,
                                     std::uint32_t viewport) {
    auto& s = *state_;
    s.preparedValid = false; s.available = 0; s.observation = {};
    if (!s.generation || !s.span(controller, 0x770) || !s.span(input, 0x294)) return 0;
    Target target;
    target.controller = controller; target.input = input; target.scene = scene; target.viewport = viewport;
    target.controllerToken = s.token(controller); target.inputToken = s.token(input);
    target.world = s.read(s.base + 0x0300D940); target.generation = s.generation();
    if (!target.world || !s.localOwner(target) || !s.fingerprint(input, target.fingerprint)) return 0;
    if (!s.arrayScratch) s.arrayScratch = s.host.GuestAlloc(16, 8);
    if (!s.arrayScratch) return 0;
    s.prepared = target;
    std::vector<std::uint32_t> actions;
    if (!s.actionDefinitions(input, actions)) return 0;
    std::array<Definition, kActionCount> definitions{};
    std::array<unsigned, kActionCount> matches{};
    // One bounded name walk per frame, not one context traversal per button.
    for (const auto object : actions) {
        const auto name = s.name(object + 0x58);
        for (std::size_t i = 0; i != kActionCount; ++i) {
            const auto* wanted = shippedActionName(static_cast<Action>(i));
            if (!wanted || name != wanted) continue;
            definitions[i] = {object, s.token(object), (s.host.MemoryRead8(object + 0x54) & 1u) != 0};
            ++matches[i];
        }
    }
    for (std::size_t i = 0; i != kActionCount; ++i) {
        if (matches[i] == 1 && s.availableNow(target, definitions[i])) s.available |= std::uint64_t{1} << i;
    }
    s.observeGameplay(target);
    s.preparedValid = s.targetUnchanged(target);
    return s.preparedValid ? s.available : 0;
}
bool Ue3GameplaySink::ready() const { return state_->preparedValid; }
void Ue3GameplaySink::telemetry(Context& context) const {
    const auto& s = *state_;
    if (!s.preparedValid || context.sceneIdentity != s.prepared.scene || !s.targetUnchanged(s.prepared)) return;
    context.knownActions = s.observation.knownActions;
    context.weaponCount = s.observation.weaponCount;
    context.grenadeCount = s.observation.grenadeCount;
    context.hudValidMask = s.observation.hudValidMask;
    context.mapRect = s.observation.mapRect;
    context.ammoRect = s.observation.ammoRect;
    context.objectiveRect = s.observation.objectiveRect;
    context.useRect = s.observation.useRect;
}
void Ue3GameplaySink::expirePendingAxes() {
    state_->movementPending = state_->lookPending = false;
    state_->right = state_->forward = state_->yaw = state_->pitch = 0;
}
void Ue3GameplaySink::beginTouchFrame(const Frame& frame) {
    auto& s = *state_;
    s.frameValid = false;
    if (frame.resetReasons) s.releaseAll();
    s.frameEpoch = frame.epoch; s.frameTarget = s.prepared;
    s.frameValid = s.preparedValid && frame.context.sceneIdentity == s.prepared.scene &&
        (frame.context.capabilities & GameplayInput) && !(frame.context.capabilities & PhysicalOwnsInput);
    if (!s.current()) s.releaseAll();
}
void Ue3GameplaySink::action(const ActionEdge& edge) {
    auto& s = *state_;
    const auto index = static_cast<std::size_t>(edge.action);
    if (index >= kActionCount) return;
    if (!edge.down) {
        if (s.owned[index].held && s.owned[index].epoch == edge.sourceEpoch) s.release(index);
        return;
    }
    if (s.owned[index].held || edge.sourceEpoch != s.frameEpoch || !s.current() ||
        !(s.available & actionBit(edge.action))) return;
    if (edge.action == Action::LastWeapon) {
        (void)s.selectLast(s.frameTarget);
        return;
    }
    Definition definition;
    if (!s.resolve(s.frameTarget, edge.action, definition) || !s.current()) return;
    // Record before dispatch: OnBegin can synchronously open a GFx scene.
    s.owned[index] = {s.frameTarget, definition, edge.sourceEpoch, true};
    if (!s.invoke(s.frameTarget, definition, true)) s.owned[index] = {};
}
void Ue3GameplaySink::pointer(const PointerEvent&) {
    // DirectUi is never advertised by this gameplay stage. Calling Native_Touch
    // and Native_Mouse together would double-submit Scaleform mouse clicks.
}
void Ue3GameplaySink::movement(float right, float forward) {
    auto& s = *state_;
    s.movementPending = false;
    if (!s.current() || !std::isfinite(right) || !std::isfinite(forward)) return;
    const auto length = std::hypot(right, forward);
    if (length > 1) { right /= length; forward /= length; }
    s.right = right; s.forward = forward; s.movementPending = true;
}
void Ue3GameplaySink::relativeLook(float yaw, float pitch) {
    auto& s = *state_;
    s.lookPending = false;
    if (!s.current() || !std::isfinite(yaw) || !std::isfinite(pitch)) return;
    const auto snapshot = sharedMailbox().snapshot();
    if (!(snapshot.context.capabilities & RelativeLook)) return;
    s.yaw = std::clamp(yaw, -kMaxPendingLookDegrees, kMaxPendingLookDegrees);
    s.pitch = std::clamp(pitch, -kMaxPendingLookDegrees, kMaxPendingLookDegrees);
    s.lookPending = true;
}
void Ue3GameplaySink::beforeAxisProcessing(std::uint32_t input) {
    auto& s = *state_;
    if (!s.movementPending || input != s.frameTarget.input) return;
    s.movementPending = false;
    // Single native add after joystick/key queue processing, before PI's normal
    // movement scaling. No second dead zone or virtual joystick callback.
    if (!s.axis(1, 0x100, s.forward) || !s.axis(7, 0x118, s.right)) s.lookPending = false;
}
void Ue3GameplaySink::beforeLookClamp(std::uint32_t input) {
    auto& s = *state_;
    if (!s.lookPending || input != s.frameTarget.input) return;
    s.lookPending = false;
    // Bytecode proof: UpdateRotation assigns int(aTurn/aLookUp) to DeltaRot.
    // This seam is after dt/FOV/mouse sensitivity and before native ignore-look
    // checks. UI, pawn, vehicle and pitch constraints remain in ProcessViewRotation.
    (void)s.axis(6, 0x114, s.yaw * kRotatorUnitsPerDegree);
    (void)s.axis(9, 0x120, s.pitch * kRotatorUnitsPerDegree);
}
void Ue3GameplaySink::retireInput(std::uint32_t input) {
    auto& s = *state_;
    for (auto& action : s.owned) if (action.target.input == input) action = {};
    if (s.frameTarget.input == input) { s.frameValid = false; expirePendingAxes(); }
    if (s.prepared.input == input) s.preparedValid = false;
}
void Ue3GameplaySink::endTouchFrame(const Frame&) {
    // OnBegin might have changed input context/movie halfway through this frame.
    if (!state_->current()) state_->releaseAll();
}

} // namespace bl2::touch

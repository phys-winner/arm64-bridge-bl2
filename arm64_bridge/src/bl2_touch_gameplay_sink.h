#pragma once
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include "bl2_touch_gfx_values.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

class DynarmicHost;

namespace bl2::touch {

struct UseAvailability { bool known = false, usable = false; };
// Use and pickup are separate original controller paths. A failed observation
// of one path must not turn the other path's empty result into an unavailable
// action. A witnessed actionable path is sufficient; both must be known empty
// before the button is removed.
inline UseAvailability Bl2CombineUseAvailability(UseAvailability use,UseAvailability pickup) {
    if ((use.known && use.usable) || (pickup.known && pickup.usable)) return {true,true};
    return {use.known && pickup.known,false};
}

inline bool Bl2UseHintFrame(double frame) {
    // Authored tools sprite602: desc572 persists at depth3 from frame2 to58.
    // Smash is Melee; cannotUse/full and trade's different desc are not Use hits.
    return std::isfinite(frame) && frame==std::floor(frame) &&
        (frame==2 || (frame>=4 && frame<=13) || frame==55);
}
inline bool Bl2UseHintLocalRect(const std::array<double,7>& v,GfxRect& out) {
    out={};
    for (const auto value:v) if (!std::isfinite(value)) return false;
    // EditText572 has AutoSize OFF, own [-40,-40,6984,573] twips. Runtime
    // translation/scale belong to desc's parent; do not treat x/y as its origin.
    if (std::abs(v[0])>=1000000 || std::abs(v[1])>=1000000 ||
        v[4]<=0 || v[4]>1000 || v[5]<=0 || v[5]>1000 || v[6]!=0 ||
        std::abs(v[2]-351.2*v[4]/100)>.02 || std::abs(v[3]-30.65*v[5]/100)>.02) return false;
    out={v[0]-2*v[4]/100,v[1]-2*v[5]/100,v[0]+349.2*v[4]/100,v[1]+28.65*v[5]/100};
    return true;
}
struct UseHintObservation {
    const char* reason="unobserved";
    double frame=0,currentFrame=0,depth=0;
    std::uint32_t toolsType=0,descType=0;
    GfxIdentity tools{},parent{};
    std::array<double,7> metrics{};
    GfxRect local{},stage{};
};
// Authored SWF display-list depth is not an AS2 dynamic-depth argument.
// Original getInstanceAtDepth (0296F140) adds 0x4000 before native lookup;
// getDepth (0291804C) subtracts the same bias. PlaceObject3 depth3 is -16381.
constexpr double kBl2UseHintAsDepth=3-0x4000;
inline bool ObserveBl2UseHint(GfxSession& session,GfxHandle tools,GfxRect& out,
                             UseHintObservation* observation=nullptr) {
    out={};
    if (observation) *observation={};
    const auto reject=[&](const char* reason) {
        if (observation) observation->reason=reason;
        return false;
    };
    const auto number=[&](GfxHandle object,const char* key,double& value) {
        const auto member=session.member(object,key);
        const bool okay=member && session.number(member,value);session.release(member);return okay;
    };
    const auto visible=[&](GfxHandle object) {
        const auto member=session.member(object,"_visible");bool enabled=false;double alpha=0;
        const bool okay=member && session.boolean(member,enabled) && enabled;
        session.release(member);
        return okay && number(object,"_alpha",alpha) && alpha>0 && alpha<=100;
    };
    const auto toolsType=session.type(tools);
    if (observation) observation->toolsType=toolsType;
    if (toolsType!=10) return reject("tools-type");
    if (!visible(tools)) return reject("tools-visibility");
    double frame=0;
    if (!number(tools,"_currentframe",frame)) return reject("tools-frame-value");
    if (observation) observation->frame=frame;
    if (!Bl2UseHintFrame(frame)) return reject("tools-frame");
    // EditText572 binds VariableName="desc" as well as instanceName="desc".
    // Its named property is therefore a STRING in the real HUD. The original
    // MovieClip display-list getter resolves the actual authored depth3 field
    // using its negative AS2 depth, independently of that bound variable;
    // never cast the string to a field or inspect the dynamic depth16387.
    const auto at=session.number(kBl2UseHintAsDepth);
    const auto desc=at ? session.invoke(tools,"getInstanceAtDepth",{at}) : GfxHandle{};
    session.release(at);
    if (observation) observation->descType=session.type(desc);
    if (session.type(desc)!=8 && session.type(desc)!=10) {
        session.release(desc);return reject("desc-display-depth");
    }
    const auto parent=session.member(desc,"_parent"),text=session.member(desc,"text"),name=session.member(desc,"_name");
    const auto depth=session.invoke(desc,"getDepth");
    GfxIdentity expected,actual,field;std::string caption,instance;double fieldDepth=0;
    bool okay=visible(desc) && session.identity(desc,field) && session.identity(tools,expected) &&
        session.identity(parent,actual) && expected==actual && session.string(name,instance,32) && instance=="desc" &&
        session.number(depth,fieldDepth) && fieldDepth==kBl2UseHintAsDepth && text && session.string(text,caption,1024) &&
        caption.find_first_not_of(" \t\r\n")!=std::string::npos;
    if (observation) {
        observation->tools=expected;observation->parent=actual;observation->depth=fieldDepth;
        observation->reason=okay ? "desc-metrics" : "desc-parent-text-visibility";
    }
    session.release(depth);session.release(name);session.release(text);session.release(parent);
    std::array<double,7> metrics{};
    constexpr const char* names[]{"_x","_y","_width","_height","_xscale","_yscale","_rotation"};
    for (std::size_t i=0;okay && i<metrics.size();++i) okay=number(desc,names[i],metrics[i]);
    if (observation) observation->metrics=metrics;
    GfxRect local,stage;double currentFrame=0;
    if (okay) {
        okay=Bl2UseHintLocalRect(metrics,local);
        if (observation) observation->reason=okay ? "projection" : "desc-shape";
    }
    if (okay) {
        okay=session.projectedRect(tools,local,stage);
        if (observation && okay) observation->reason="frame-visibility-recheck";
    }
    okay=okay && number(tools,"_currentframe",currentFrame) && currentFrame==frame && visible(tools) && visible(desc);
    if (okay) {
        const auto atNow=session.number(kBl2UseHintAsDepth);
        const auto fresh=atNow ? session.invoke(tools,"getInstanceAtDepth",{atNow}) : GfxHandle{};
        GfxIdentity freshField;okay=session.identity(fresh,freshField) && freshField==field;
        session.release(fresh);session.release(atNow);
        if (!okay && observation) observation->reason="desc-display-replaced";
    }
    if (observation) {
        observation->local=local;observation->stage=stage;observation->currentFrame=currentFrame;
    }
    session.release(desc);
    if (!okay || !session.good()) return false;
    if (observation) observation->reason="projected";
    out=stage;return true;
}

// This history observes the real equipped weapon, independent of touch input.
// The game's LastUsedWeapon field is a restriction/restoration cache, so it is
// deliberately not used as a previous-weapon command. A holstered/null weapon
// is not a switch. Owner changes retire the complete history.
struct EquippedWeaponHistory {
    std::uint64_t controller = 0, pawn = 0, current = 0, previous = 0;
    std::uint32_t world = 0;
    void observe(std::uint64_t owner, std::uint64_t ownerPawn, std::uint32_t ownerWorld,
                 std::uint64_t weapon) {
        if (!owner || !ownerPawn || !ownerWorld) { *this = {}; return; }
        if (controller != owner || pawn != ownerPawn || world != ownerWorld) {
            *this = {}; controller = owner; pawn = ownerPawn; world = ownerWorld;
        }
        if (!weapon || weapon == current) return;
        previous = current; current = weapon;
    }
    std::uint64_t candidate(std::uint64_t owner, std::uint64_t ownerPawn,
                            std::uint32_t ownerWorld, std::uint64_t equipped) const {
        return controller == owner && pawn == ownerPawn && world == ownerWorld &&
            current && current == equipped && previous != equipped ? previous : 0;
    }
};

// Current TextField attributes [x,y,width,height,xscale,yscale,rotation]. The
// fixed non-autosized EditText671/955 owns [1573,5347,-40,731] twips. Preserve
// that nonzero origin while projecting through its real parent MovieClip.
inline bool Bl2AmmoCounterLocalRect(const std::array<double,7>& v,std::array<double,4>& out) {
    out={};
    for (const auto value:v) if (!std::isfinite(value)) return false;
    if (std::abs(v[0])>=1000000 || std::abs(v[1])>=1000000 ||
        v[4]<=0 || v[4]>1000 || v[5]<=0 || v[5]>1000 || v[6]!=0 ||
        std::abs(v[2]-188.7*v[4]/100)>.02 || std::abs(v[3]-38.55*v[5]/100)>.02) return false;
    out={v[0]+78.65*v[4]/100,v[1]-2*v[5]/100,v[0]+267.35*v[4]/100,v[1]+36.55*v[5]/100};
    return true;
}

// Concrete semantic gameplay delivery for the immutable A0B06E... guest.
// Construction does not install hooks and does not enable capabilities. The
// runtime provider must independently validate the image and live context.
// All methods below run under the host engine-input mutex, never on Android UI.
class Ue3GameplaySink final : public EngineSink {
public:
    using LifetimeGeneration = std::uint64_t (*)();
    Ue3GameplaySink(DynarmicHost& host, LifetimeGeneration generation);
    ~Ue3GameplaySink() override;
    Ue3GameplaySink(const Ue3GameplaySink&) = delete;
    Ue3GameplaySink& operator=(const Ue3GameplaySink&) = delete;

    // Call before publishContext/consumeIntoEngine. Captures a bounded live
    // controller/input/world/focused-movie/context identity, and independently
    // resolves actions through InputActionDefinition + IsActionAvailable.
    // Returns semantic action bits, or zero on malformed/ambiguous data.
    std::uint64_t prepare(std::uint32_t controller, std::uint32_t input,
                          std::uint64_t sceneIdentity, std::uint32_t viewport);
    bool ready() const;
    // Read-only, input-boundary observations. Unknown counts remain -1; bounds
    // are advertised individually only after native HUD projection succeeds.
    void telemetry(Context& context) const;
    // No pending payload may survive an input tick without its native consumer.
    void expirePendingAxes();
    // Hooks inside the same input tick. They preserve the native processing:
    // PIProcessInput entry: normalized movement before game scaling.
    // PIPostAdjustInput +0x114: degrees to Rotator units before ignore clamps.
    void beforeAxisProcessing(std::uint32_t input);
    void beforeLookClamp(std::uint32_t input);
    // Destructor hook: never call methods on an object being destroyed.
    void retireInput(std::uint32_t input);

    void beginTouchFrame(const Frame& frame) override;
    void action(const ActionEdge& edge) override;
    void pointer(const PointerEvent& event) override;
    void movement(float right, float forward) override;
    void relativeLook(float yawDegrees, float pitchDegrees) override;
    void endTouchFrame(const Frame& frame) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bl2::touch

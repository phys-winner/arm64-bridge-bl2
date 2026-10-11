#pragma once
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include "bl2_touch_gfx_values.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

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
// EditText572 is left aligned: a short caption paints only its first
// textWidth pixels after the 2px gutter. Keep that painted part as the tap
// target instead of the empty rest of the 351px box over the look area.
// An unreadable or implausible width keeps the whole authored box.
inline GfxRect Bl2UseHintCaptionRect(const GfxRect& field,const std::array<double,7>& v,double textWidth) {
    if (!std::isfinite(textWidth) || textWidth<=0 || textWidth>347.2) return field;
    auto out=field;
    out.right=std::min(field.right,v[0]+(textWidth+4)*v[4]/100);
    return out;
}
struct UseHintObservation {
    const char* reason="unobserved";
    double frame=0,currentFrame=0,depth=0,textWidth=-1;
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
        double textWidth=-1;
        if (okay && number(desc,"textWidth",textWidth)) local=Bl2UseHintCaptionRect(local,metrics,textWidth);
        if (observation) { observation->reason=okay ? "projection" : "desc-shape";observation->textWidth=textWidth; }
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
// fixed non-autosized EditText671/955 owns [1573,5347,-40,731] twips. Scaleform
// reports a TextField's _x/_y at that own rectangle's top-left, not at its
// placement: the field placed at (-271.9,-7.05) reads (-193.25,-9.05) on
// device. Never add the authored origin a second time.
inline bool Bl2AmmoCounterLocalRect(const std::array<double,7>& v,std::array<double,4>& out) {
    out={};
    for (const auto value:v) if (!std::isfinite(value)) return false;
    if (std::abs(v[0])>=1000000 || std::abs(v[1])>=1000000 ||
        v[4]<=0 || v[4]>1000 || v[5]<=0 || v[5]>1000 || v[6]!=0 ||
        std::abs(v[2]-188.7*v[4]/100)>.02 || std::abs(v[3]-38.55*v[5]/100)>.02) return false;
    out={v[0],v[1],v[0]+v[2],v[1]+v[3]};
    return true;
}
// The whole visible ammo strip in parent p1.bullets coordinates (HUD
// sprite673, frames none..weaponSwitch): bar backdrop604/672, the fill
// 643/647 inside mask641, separator252, counter EditText671 and the union of
// every bullet_icon670 frame. The flash effect is excluded. These children
// keep authored placements; prove that with the counter's exact reported
// rectangle origin, else keep only the counter as the target.
inline bool Bl2AmmoStripLocalRect(const std::array<double,7>& v,std::array<double,4>& out) {
    out={};
    for (const auto value:v) if (!std::isfinite(value)) return false;
    if (std::abs(v[0]+193.25)>.05 || std::abs(v[1]+9.05)>.05 || std::abs(v[2]-188.7)>.02 ||
        std::abs(v[3]-38.55)>.02 || std::abs(v[4]-100)>.01 || std::abs(v[5]-100)>.01 || v[6]!=0) return false;
    out={-212.85,-15.25,56.15,36.6};
    return true;
}

// Protocol 5 HUD tap targets. A clip counts only while it and its alpha are
// shown; metrics are [x,y,width,height,xscale,yscale,rotation] in the parent.
inline bool Bl2HudClipShown(GfxSession& session,GfxHandle clip) {
    if (session.type(clip)!=10) return false;
    const auto v=session.member(clip,"_visible"),a=session.member(clip,"_alpha");
    bool shown=false;double alpha=0;
    const bool okay=v && a && session.boolean(v,shown) && shown && session.number(a,alpha) && alpha>0 && alpha<=100;
    session.release(v);session.release(a);return okay;
}
inline bool Bl2HudClipMetrics(GfxSession& session,GfxHandle clip,std::array<double,7>& v) {
    constexpr const char* names[]{"_x","_y","_width","_height","_xscale","_yscale","_rotation"};
    for (std::size_t i=0;i<v.size();++i) {
        const auto value=session.member(clip,names[i]);
        const bool okay=value && session.number(value,v[i]) && std::isfinite(v[i]);
        session.release(value);
        if (!okay) return false;
    }
    return true;
}
// p1.character is the action skill portrait (UpdateActionSkill/-Portrait).
// Its frames 1-6 hold `cooldown` (sprite399): the skill image is loaded into
// cooldown.icon (sprite384), and circle shape378 [-2.05,37.8]x[-4.65,35.05]
// sits at (-0.5,3.1); both stay on all 101 cooldown frames. The "_none"
// frames 7-11 and the "vehicle" frame 12 have no cooldown clip. The icon's
// authored unscaled placement (16.7,17.95) proves that layout.
constexpr GfxRect kBl2SkillIconLocal{-2.55,-1.55,37.3,38.15};
inline bool Bl2SkillIconPlacement(const std::array<double,7>& icon) {
    return std::abs(icon[0]-16.7)<=.05 && std::abs(icon[1]-17.95)<=.05 &&
        std::abs(icon[4]-100)<=.01 && std::abs(icon[5]-100)<=.01 && icon[6]==0;
}
inline bool ObserveBl2SkillIcon(GfxSession& session,GfxHandle character,GfxRect& out) {
    out={};
    if (!Bl2HudClipShown(session,character)) return false;
    const auto cooldown=session.member(character,"cooldown"),icon=session.member(cooldown,"icon");
    std::array<double,7> metrics{};
    const bool okay=Bl2HudClipShown(session,cooldown) && session.type(icon)==10 &&
        Bl2HudClipMetrics(session,icon,metrics) && Bl2SkillIconPlacement(metrics) &&
        session.projectedRect(cooldown,kBl2SkillIconLocal,out);
    session.release(icon);session.release(cooldown);
    if (okay && session.good()) return true;
    out={};return false;
}
// p1.xpbar (sprite191, 100 progress frames): background shape183
// [-200.5,200.5]x[-6.9,20.1] stays at depth1 on every frame. The fill morph
// char185 ([-195.7,-190.45..193.3]x[-6.9,7.1]), frame-100 art and xpbkgd lie
// inside it; only the "go" flash reaches y=-8. Unscaled width 401 and height
// 27..28.1 prove that art.
constexpr GfxRect kBl2ExperienceBarLocal{-200.5,-6.9,200.5,20.1};
inline bool Bl2ExperienceBarShape(const std::array<double,7>& v) {
    if (v[4]<=0 || v[4]>1000 || v[5]<=0 || v[5]>1000 || v[6]!=0) return false;
    const double width=v[2]*100/v[4],height=v[3]*100/v[5];
    return std::abs(width-401)<=.2 && height>=26.8 && height<=28.3;
}
inline bool ObserveBl2ExperienceBar(GfxSession& session,GfxHandle bar,GfxRect& out) {
    out={};
    std::array<double,7> metrics{};
    const bool okay=Bl2HudClipShown(session,bar) && Bl2HudClipMetrics(session,bar,metrics) &&
        Bl2ExperienceBarShape(metrics) && session.projectedRect(bar,kBl2ExperienceBarLocal,out);
    if (okay && session.good()) return true;
    out={};return false;
}
// p1.challenges (sprite851): frame 1 "off", 2-13 slide `inner` in (x -240..0,
// alpha 0..100%) and stop, 14-24 "out". inner.bkgd (sprite842, authored at
// (-16.75,-27)) holds two shape841 copies spanning [0,254]x[-26,27.5] behind
// the header and subtext; the subtext clip's frame-2 script stretches it with
// _parent.bkgd._width from the caption's textWidth, i.e. through _xscale.
// Frames 8-13 are at least 79% opaque and within 50px of rest; the sliding or
// fading notice is not a target.
constexpr GfxRect kBl2ChallengeLocal{0,-26,254,27.5};
inline bool Bl2ChallengeFrame(double frame) {
    return std::isfinite(frame) && frame==std::floor(frame) && frame>=8 && frame<=13;
}
inline bool Bl2ChallengeBackground(const std::array<double,7>& v) {
    if (v[4]<=0 || v[4]>1000 || std::abs(v[5]-100)>.01 || v[6]!=0) return false;
    return std::abs(v[0]+16.75)<=.05 && std::abs(v[1]+27)<=.05 &&
        std::abs(v[2]*100/v[4]-254)<=.2 && std::abs(v[3]-53.5)<=.2;
}
inline bool ObserveBl2ChallengeNotice(GfxSession& session,GfxHandle challenges,GfxRect& out) {
    out={};
    if (!Bl2HudClipShown(session,challenges)) return false;
    const auto frame=session.member(challenges,"_currentframe");
    double current=0;
    const bool active=frame && session.number(frame,current) && Bl2ChallengeFrame(current);
    session.release(frame);
    if (!active) return false;
    const auto inner=session.member(challenges,"inner"),background=session.member(inner,"bkgd");
    std::array<double,7> metrics{};
    const bool okay=Bl2HudClipShown(session,inner) && Bl2HudClipShown(session,background) &&
        Bl2HudClipMetrics(session,background,metrics) && Bl2ChallengeBackground(metrics) &&
        session.projectedRect(background,kBl2ChallengeLocal,out);
    session.release(background);session.release(inner);
    if (okay && session.good()) return true;
    out={};return false;
}

// Context::weaponSlots from the readied weapons. QuickSelectSlot is
// EQuickWeaponSlot (QuickSelectUp 1 .. QuickSelectRight 4: the Slot1..Slot4
// actions); type is EWeaponType + 1, or 0 when unreadable. The original
// command addresses a slot, not an item: a duplicate or out-of-range slot, or
// two equipped weapons, leaves the word unknown (0).
struct WeaponSlotEntry { std::uint8_t slot = 0, type = 0; bool equipped = false; };
inline std::uint32_t Bl2WeaponSlotsWord(const std::vector<WeaponSlotEntry>& entries) {
    std::uint32_t word=kWeaponSlotsKnown;
    for (const auto& entry:entries) {
        if (entry.slot<1 || entry.slot>4 || entry.type>6) return 0;
        const auto bit=1u<<(entry.slot-1);
        if ((word & bit) || (entry.equipped && (word & 0x700u))) return 0;
        word|=bit | (std::uint32_t{entry.type}<<(16+4*(entry.slot-1)));
        if (entry.equipped) word|=std::uint32_t{entry.slot}<<8;
    }
    return word;
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
    // Without axes (Assassin Execute, cinematics) movement and look are dropped.
    // Gameplay observes HUD/weapon/Use/skill state; a vehicle seat observes
    // nothing and its movement also feeds RawJoyUp/RawJoyRight; a cinematic
    // observes only the original SkipMatinee rule.
    enum class Scene : std::uint8_t { Gameplay, Vehicle, Cinematic };
    std::uint64_t prepare(std::uint32_t controller, std::uint32_t input,
                          std::uint64_t sceneIdentity, std::uint32_t viewport, bool axes, Scene scene);
    bool ready() const;
    // Read-only, input-boundary observations. Unknown counts remain -1; bounds
    // are advertised individually only after native HUD projection succeeds.
    void telemetry(Context& context) const;
    // No pending payload may survive an input tick without its native consumer.
    void expirePendingAxes();
    // Hooks inside the same input tick. They preserve the native processing:
    // PIProcessInput entry: normalized movement before game scaling; in a
    // vehicle seat also the raw stick PlayerDriving reads after this seam.
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

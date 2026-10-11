#include "bl2_touch_gameplay_sink.h"
#include "bl2_touch_runtime.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using namespace bl2::touch;
int main() {
    using Names=std::vector<std::string>;
    // Globals.InputContexts and CharClass_*.InputContexts of Startup.xxx.
    const Names onFoot{"InputContext_OnFoot","InputContext_Injured","InputContext_InjuredMoving",
        "InputContext_SirenOnFoot","InputContext_SirenPhaselock","InputContext_SoldierScorpio",
        "InputContext_MercenaryGunzerking","InputContext_MercenaryGunzerkingInjured",
        "InputContext_MercenaryInjured","InputContext_MercenaryInjuredMoving","InputContext_AssassinVisionMode"};
    const Names vehicle{"InputContext_DrivingVehicle","InputContext_InVehicleTurret","InputContext_VehicleAttachedRider"};
    assert(onFoot.size()+vehicle.size()+2==sizeof(kBl2TouchContexts)/sizeof(kBl2TouchContexts[0]));
    for (const auto& name:onFoot) {
        const Names one{name};
        assert(Bl2TouchContextMode(one)==Mode::Gameplay);
        assert(Bl2TouchGameplayActions(Mode::Gameplay,0,one) && Bl2TouchGameplayAxes(Mode::Gameplay,0,one));
        // A matched name cannot authorize a focused native menu or a changed mode.
        assert(!Bl2TouchGameplayActions(Mode::Gameplay,0x10000,one) && !Bl2TouchGameplayAxes(Mode::Gameplay,0x10000,one));
        for (const auto mode:{Mode::Unknown,Mode::Cinematic,Mode::Vehicle,Mode::Menu,Mode::Dialog,Mode::Dead})
            assert(!Bl2TouchGameplayActions(mode,0,one) && !Bl2TouchGameplayAxes(mode,0,one));
        assert(!Bl2TouchVehicleActions(Mode::Gameplay,0,one) && !Bl2TouchVehicleActions(Mode::Vehicle,0,one));
    }
    // Siren: the class OnFoot entry joins the global one. Mercenary FFYL may
    // keep or replace the global injured context; both shapes are admitted.
    for (const auto& pair:{Names{"InputContext_OnFoot","InputContext_SirenOnFoot"},
            Names{"InputContext_SirenOnFoot","InputContext_OnFoot"},
            Names{"InputContext_Injured","InputContext_MercenaryInjured"},
            Names{"InputContext_InjuredMoving","InputContext_MercenaryInjuredMoving"},
            Names{"InputContext_Injured","InputContext_SirenOnFoot"}}) {
        assert(Bl2TouchContextMode(pair)==Mode::Gameplay);
        assert(Bl2TouchGameplayActions(Mode::Gameplay,0,pair) && Bl2TouchGameplayAxes(Mode::Gameplay,0,pair));
    }
    // Execute is gameplay with actions only: no movement set, no axes.
    for (const auto& execute:{Names{"InputContext_AssassinExecute"},
            Names{"InputContext_OnFoot","InputContext_AssassinExecute"}}) {
        assert(Bl2TouchContextMode(execute)==Mode::Gameplay);
        assert(Bl2TouchGameplayActions(Mode::Gameplay,0,execute) && !Bl2TouchGameplayAxes(Mode::Gameplay,0,execute));
    }
    for (const auto& name:vehicle) {
        assert(Bl2TouchContextMode(Names{name})==Mode::Vehicle);
        assert(Bl2TouchContextMode(Names{"InputContext_OnFoot",name})==Mode::Vehicle);
        assert(Bl2TouchContextMode(Names{"InputContext_Cinematic",name})==Mode::Cinematic);
        assert(!Bl2TouchGameplayActions(Mode::Vehicle,0,Names{name}) && !Bl2TouchGameplayAxes(Mode::Vehicle,0,Names{name}));
        // Seats route through their own gate with axes, never under a focused
        // movie, another mode, or a cinematic that outranks the seat.
        assert(Bl2TouchVehicleActions(Mode::Vehicle,0,Names{name}) && Bl2TouchVehicleAxes(Mode::Vehicle,0,Names{name}));
        assert(Bl2TouchVehicleActions(Mode::Vehicle,0,Names{"InputContext_OnFoot",name}));
        assert(!Bl2TouchVehicleActions(Mode::Vehicle,0x10000,Names{name}) && !Bl2TouchVehicleAxes(Mode::Vehicle,0x10000,Names{name}));
        for (const auto mode:{Mode::Unknown,Mode::Gameplay,Mode::Cinematic,Mode::Menu,Mode::Dialog,Mode::Dead})
            assert(!Bl2TouchVehicleActions(mode,0,Names{name}) && !Bl2TouchVehicleAxes(mode,0,Names{name}));
        assert(!Bl2TouchVehicleActions(Mode::Vehicle,0,Names{"InputContext_Cinematic",name}));
        assert(!Bl2TouchCinematicActions(Mode::Vehicle,0,Names{name}));
    }
    assert(Bl2TouchContextMode(Names{"InputContext_Cinematic"})==Mode::Cinematic);
    assert(Bl2TouchContextMode(Names{"InputContext_SirenOnFoot","InputContext_Cinematic"})==Mode::Cinematic);
    assert(!Bl2TouchGameplayAxes(Mode::Cinematic,0,Names{"InputContext_Cinematic"}));
    // Cinematic Skip/Pause: a listed cinematic without a focused movie, never gameplay.
    assert(Bl2TouchCinematicActions(Mode::Cinematic,0,Names{"InputContext_Cinematic"}));
    assert(Bl2TouchCinematicActions(Mode::Cinematic,0,Names{"InputContext_OnFoot","InputContext_Cinematic"}));
    assert(!Bl2TouchCinematicActions(Mode::Cinematic,0x10000,Names{"InputContext_Cinematic"}));
    assert(!Bl2TouchCinematicActions(Mode::Menu,0,Names{"InputContext_Cinematic"}));
    assert(!Bl2TouchCinematicActions(Mode::Gameplay,0,Names{"InputContext_OnFoot"}));
    assert(!Bl2TouchCinematicActions(Mode::Cinematic,0,Names{"InputContext_Cinematic","InputContext_InjuredClient"}));
    assert(!Bl2TouchGameplayActions(Mode::Cinematic,0,Names{"InputContext_Cinematic"}));
    // Empty, three or more, and foreign/fixture names (absent from the shipped
    // data, compared case-sensitively) publish nothing.
    for (const auto& rejected:{Names{},Names{"InputContext_OnFoot","InputContext_SirenOnFoot","InputContext_Injured"},
            Names{"InputContext_InjuredClient"},Names{"InputContext_InjuredMovingOther"},Names{""},
            Names{"InputContext_onfoot"},Names{"DefaultOnFoot"},Names{"InputContext_OnFoot","InputContext_InjuredClient"},
            Names{"InputContext_InjuredClient","InputContext_Cinematic"}}) {
        assert(Bl2TouchContextMode(rejected)==Mode::Unknown);
        assert(!Bl2TouchGameplayActions(Mode::Gameplay,0,rejected) && !Bl2TouchGameplayAxes(Mode::Gameplay,0,rejected));
        assert(!Bl2TouchVehicleActions(Mode::Vehicle,0,rejected) && !Bl2TouchVehicleAxes(Mode::Vehicle,0,rejected));
    }
    assert(!Bl2TouchVehicleActions(Mode::Vehicle,0,Names{"InputContext_DrivingVehicle","InputContext_InjuredClient"}));
    std::cout << "PASS: 16 shipped contexts, Siren/Mercenary pairs, Execute without axes, vehicle/cinematic modes, "
                 "vehicle seat route with axes, cinematic Skip/Pause route, focused/mixed/empty/3+/foreign context rejection\n";
    const auto empty=Bl2CombineUseAvailability({true,false},{true,false});
    assert(empty.known && !empty.usable);
    const auto loot=Bl2CombineUseAvailability({true,false},{true,true});
    assert(loot.known && loot.usable); // Empty trigger lists do not hide genuine loot.
    const auto trigger=Bl2CombineUseAvailability({true,true},{false,false});
    assert(trigger.known && trigger.usable);
    assert(!Bl2CombineUseAvailability({true,false},{false,false}).known);
    assert(!Bl2CombineUseAvailability({false,false},{true,false}).known);
    assert(!Bl2CombineUseAvailability({false,false},{false,false}).known);
    const auto foreign=Bl2CombineUseAvailability({false,false},{true,true});
    assert(foreign.known && foreign.usable);
    EquippedWeaponHistory h;
    constexpr std::uint64_t a=0x2000000000000001ull,b=0x2000100000000002ull,c=0x2000200000000003ull;
    h.observe(11,22,33,a);
    assert(!h.candidate(11,22,33,a));
    h.observe(11,22,33,0); // Holster/loading is not another weapon.
    h.observe(11,22,33,a);
    assert(!h.candidate(11,22,33,a));
    h.observe(11,22,33,b);
    assert(h.candidate(11,22,33,b)==a);
    h.observe(11,22,33,c); // Three weapons prove this cannot alias PreviousWeapon.
    assert(h.candidate(11,22,33,c)==b);
    h.observe(11,22,33,b); // The game's original selection has actually completed.
    assert(h.candidate(11,22,33,b)==c);
    assert(!h.candidate(11,22,33,a)); // A new native swap after sampling is rejected.
    assert(!h.candidate(11,222,33,b));
    h.observe(11,222,33,a); // Respawn retires another pawn's history.
    assert(!h.candidate(11,222,33,a));
    h.observe(11,222,44,b); // World changes retire history even for the same address.
    assert(!h.candidate(11,222,44,b));
    h.observe(111,222,44,c);
    assert(!h.candidate(111,222,44,c));
    h.observe(0,222,44,b);
    assert(!h.current && !h.previous && !h.controller);
    std::array<double,4> rect{};
    for (const auto scale:{1.0,1.25,2.0}) {
        // Scaleform's TextField _x/_y already name the own rectangle's top-left.
        const std::array<double,7> metrics{-193.25,-9.05,188.7*scale,38.55*scale,100*scale,100*scale,0};
        assert(Bl2AmmoCounterLocalRect(metrics,rect));
        assert(std::abs(rect[0]-(-193.25))<1e-9);
        assert(std::abs(rect[1]-(-9.05))<1e-9);
        assert(std::abs(rect[2]-(-193.25+188.7*scale))<1e-9);
        assert(std::abs(rect[3]-(-9.05+38.55*scale))<1e-9);
    }
    // Device-reported metrics of EditText671 placed at (-271.9,-7.05).
    const std::array<double,7> authored{-193.25,-9.05,188.7,38.55,100,100,0};
    for (const unsigned invalid:{2u,3u,4u,5u,6u}) {
        auto metrics=authored;metrics[invalid]=0;
        if (invalid==6) metrics[invalid]=.001;
        assert((!Bl2AmmoCounterLocalRect(metrics,rect) && rect==std::array<double,4>{}));
    }
    auto dead=authored;dead[0]=std::numeric_limits<double>::quiet_NaN();
    assert(!Bl2AmmoCounterLocalRect(dead,rect));
    // The whole authored strip: backdrop, masked fill, counter and every icon frame.
    std::array<double,4> strip{};
    assert(Bl2AmmoStripLocalRect(authored,strip) && strip==(std::array<double,4>{-212.85,-15.25,56.15,36.6}));
    assert(Bl2AmmoCounterLocalRect(authored,rect));
    assert(strip[0]<rect[0] && strip[1]<rect[1] && strip[2]>rect[2] && strip[3]>rect[3]);
    for (const unsigned moved:{0u,1u,4u,5u,6u}) {
        auto metrics=authored;metrics[moved]+=moved==6 ? .01 : 1;
        if (moved>=4) {metrics[2]*=metrics[4]/100;metrics[3]*=metrics[5]/100;}
        // A moved or rescaled counter proves only itself; never a guessed strip.
        assert((!Bl2AmmoStripLocalRect(metrics,strip) && strip==std::array<double,4>{}));
    }
    // The SWF placement translation is not what Scaleform reports: a counter
    // that really reads (-271.9,-7.05) has moved 78.65px left and proves no strip.
    auto placement=authored;placement[0]=-271.9;placement[1]=-7.05;
    assert((!Bl2AmmoStripLocalRect(placement,strip) && strip==std::array<double,4>{}));
    assert(Bl2AmmoCounterLocalRect(placement,rect) && rect[0]==-271.9 && rect[2]==-271.9+188.7);
    assert(!Bl2AmmoStripLocalRect(dead,strip));
    std::cout << "PASS: whole ammo strip only around the reported counter rectangle origin\n";
    std::cout << "PASS: equipped-weapon history, native switch confirmation, three-slot previous, holster and owner retirement\n";
    std::cout << "PASS: persistent ammo counter origin, 100/125/200 attributes, fixed box and malformed transform rejection\n";
    std::cout << "PASS: original trigger/pickup union, actionable loot, both-empty and unknown-path negative controls\n";
}

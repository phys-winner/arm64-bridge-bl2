#include "bl2_touch_gameplay_sink.h"
#include "bl2_touch_runtime.h"
#include <cassert>
#include <iostream>
#include <limits>
using namespace bl2::touch;
int main() {
    assert(Bl2TouchGameplayAxes(Mode::Gameplay,0,1,"InputContext_OnFoot"));
    for (const auto name:{"InputContext_Injured","InputContext_InjuredMoving"}) {
        assert(Bl2TouchGameplayAxes(Mode::Gameplay,0,1,name));
        // A matched name cannot authorize a focused native menu, a changed
        // mode, or a mixed/empty context list from the same controller.
        assert(!Bl2TouchGameplayAxes(Mode::Gameplay,0x10000,1,name));
        assert(!Bl2TouchGameplayAxes(Mode::Menu,0,1,name));
        assert(!Bl2TouchGameplayAxes(Mode::Dialog,0,1,name));
        assert(!Bl2TouchGameplayAxes(Mode::Dead,0,1,name));
        assert(!Bl2TouchGameplayAxes(Mode::Gameplay,0,0,name));
        assert(!Bl2TouchGameplayAxes(Mode::Gameplay,0,2,name));
    }
    for (const auto name:{"InputContext_InjuredClient","InputContext_InjuredMovingOther",
                         "InputContext_DrivingVehicle","InputContext_Cinematic",""})
        assert(!Bl2TouchGameplayAxes(Mode::Gameplay,0,1,name));
    std::cout << "PASS: exact shared injured axes, focused/mixed/empty/foreign context rejection\n";
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
        const std::array<double,7> metrics{-271.9,-7.05,188.7*scale,38.55*scale,100*scale,100*scale,0};
        assert(Bl2AmmoCounterLocalRect(metrics,rect));
        assert(std::abs(rect[0]-(-271.9+78.65*scale))<1e-9);
        assert(std::abs(rect[1]-(-7.05-2*scale))<1e-9);
        assert(std::abs(rect[2]-(-271.9+267.35*scale))<1e-9);
        assert(std::abs(rect[3]-(-7.05+36.55*scale))<1e-9);
    }
    const std::array<double,7> authored{-271.9,-7.05,188.7,38.55,100,100,0};
    for (const unsigned invalid:{2u,3u,4u,5u,6u}) {
        auto metrics=authored;metrics[invalid]=0;
        if (invalid==6) metrics[invalid]=.001;
        assert((!Bl2AmmoCounterLocalRect(metrics,rect) && rect==std::array<double,4>{}));
    }
    auto dead=authored;dead[0]=std::numeric_limits<double>::quiet_NaN();
    assert(!Bl2AmmoCounterLocalRect(dead,rect));
    std::cout << "PASS: equipped-weapon history, native switch confirmation, three-slot previous, holster and owner retirement\n";
    std::cout << "PASS: persistent ammo counter origin, 100/125/200 attributes, fixed box and malformed transform rejection\n";
    std::cout << "PASS: original trigger/pickup union, actionable loot, both-empty and unknown-path negative controls\n";
}

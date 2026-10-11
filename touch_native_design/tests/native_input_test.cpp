#include "bl2_touch_engine_adapter.h"
#include "bl2_touch_bindings.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>

using namespace bl2::touch;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL " << __func__ << ':' << __LINE__ << ": " #condition << '\n'; \
    std::exit(1); } } while (false)

static bool near(float a, float b) { return std::abs(a - b) < 0.0001f; }
static Context gameplay(std::uint64_t identity = 1) {
    return {Mode::Gameplay, GameplayInput | RelativeLook, kKnownActions, identity};
}
static std::uint64_t ready(Mailbox& box, Context context = gameplay()) {
    const auto epoch = box.publishContext(context);
    CHECK(box.submit(epoch, 0, 0, 0, 0, 0, 100));
    const auto barrier = box.consumeFrame(1, 100);
    CHECK(barrier.fresh && barrier.resetReasons == ContextChanged);
    return epoch;
}
static bool send(Mailbox& box, std::uint64_t epoch, std::uint64_t held,
                 std::uint64_t time = 101) {
    return box.submit(epoch, 0, 0, 0, 0, held, time);
}

static void testUnknownAndWire() {
    Mailbox box;
    const auto wire = box.snapshot().wire();
    CHECK(wire.size() == 18 && wire[0] == 4 && wire[0] == kProtocolVersion && wire[1] != 0 && wire[2] == 0 && wire[3] == 0);
    CHECK(wire[4] == 0 && wire[5] == 0 && wire[6] == 0);
    CHECK(wire[7] == std::numeric_limits<std::uint64_t>::max() && wire[8] == wire[7]);
    for (std::size_t i = 9; i != wire.size(); ++i) CHECK(wire[i] == 0);
    CHECK(!send(box, wire[1], actionBit(Action::Fire)));
    auto frame = box.consumeFrame(0, 100);
    CHECK(frame.fresh && frame.actionEdgeCount == 0 && near(frame.moveX, 0));
    CHECK(!box.consumeFrame(0, 100).fresh);
}

static float unpackFloat(std::uint64_t word, bool high) {
    const auto bits = static_cast<std::uint32_t>(high ? word >> 32 : word);
    float result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

static void testLiveTelemetryPreservesCaptureAndReleasesRevocation() {
    Mailbox box;
    auto context = gameplay();
    context.capabilities |= DirectUi;
    const auto epoch = ready(box, context);
    CHECK(box.uiPointer(epoch, 9, PointerPhase::Down, .8f, .9f, 101));
    CHECK(box.submit(epoch, .4f, .2f, 0, 0, actionBit(Action::Grenade), 101));
    box.consumeFrame(2, 101);
    context.knownActions = actionBit(Action::Grenade) | actionBit(Action::NextWeapon);
    context.weaponCount = 3; context.grenadeCount = 1;
    context.hudValidMask = HudMinimap | HudAmmo | HudObjectives;
    context.mapRect = {.75f, .05f, .95f, .35f};
    context.ammoRect = {.8f, .7f, .98f, .95f};
    context.objectiveRect = {.67f, .36f, .96f, .47f};
    CHECK(box.publishContext(context) == epoch);
    auto snap = box.snapshot();
    const auto wire = snap.wire();
    CHECK(wire[6] == context.knownActions && wire[7] == 3 && wire[8] == 1);
    CHECK(wire[9] == 7);
    CHECK(near(unpackFloat(wire[14], false), .67f) && near(unpackFloat(wire[14], true), .36f));
    CHECK(near(unpackFloat(wire[15], false), .96f) && near(unpackFloat(wire[15], true), .47f));
    CHECK(near(unpackFloat(wire[10], false), .75f) && near(unpackFloat(wire[10], true), .05f));
    CHECK(near(unpackFloat(wire[13], false), .98f) && near(unpackFloat(wire[13], true), .95f));
    CHECK(box.uiPointer(epoch, 9, PointerPhase::Move, .85f, .88f, 102));
    auto frame = box.consumeFrame(3, 102);
    CHECK(frame.resetReasons == NoReset && frame.pointerEventCount == 1);
    CHECK(frame.pointerEvents[0].phase == PointerPhase::Move && frame.context.grenadeCount == 1);
    CHECK(frame.heldActions == actionBit(Action::Grenade) && near(frame.moveX, .4f));
    context.grenadeCount = 0;
    context.availableActions &= ~actionBit(Action::Grenade);
    context.objectiveRect.top = .39f;
    CHECK(box.publishContext(context) == epoch);
    CHECK(box.uiPointer(epoch, 9, PointerPhase::Up, .85f, .88f, 103));
    frame = box.consumeFrame(4, 103);
    CHECK(frame.context.grenadeCount == 0 && near(frame.context.objectiveRect.top,.39f));
    CHECK(frame.resetReasons == NoReset && near(frame.moveX, .4f));
    CHECK(frame.actionEdgeCount == 1 && !frame.actionEdges[0].down);
    CHECK(frame.actionEdges[0].action == Action::Grenade && frame.actionEdges[0].sourceEpoch == epoch);
    CHECK(frame.pointerEventCount == 1 && frame.pointerEvents[0].phase == PointerPhase::Up);
}

static void testTelemetryRejectsEachInvalidRectIndependently() {
    Mailbox box;
    auto context = gameplay();
    const auto epoch = ready(box, context);
    context.knownActions = ~std::uint64_t{0};
    context.weaponCount = 65; context.grenadeCount = 65536;
    context.hudValidMask = ~std::uint32_t{0};
    context.mapRect = {.2f, .1f, .4f, .3f};
    context.ammoRect = {0, 0, std::numeric_limits<float>::quiet_NaN(), 1};
    CHECK(box.publishContext(context) == epoch);
    auto snap = box.snapshot();
    CHECK(snap.context.knownActions == kKnownActions && snap.context.weaponCount == -1 && snap.context.grenadeCount == -1);
    CHECK(snap.context.hudValidMask == HudMinimap && snap.wire()[12] == 0 && snap.wire()[13] == 0);
    context.mapRect = {.7f, .1f, .6f, .3f};
    context.ammoRect = {.6f, .7f, 1, 1};
    context.weaponCount = 64; context.grenadeCount = 65535;
    CHECK(box.publishContext(context) == epoch);
    snap = box.snapshot();
    CHECK(snap.context.hudValidMask == HudAmmo && snap.context.weaponCount == 64 && snap.context.grenadeCount == 65535);
    CHECK(snap.wire()[10] == 0 && snap.wire()[11] == 0);
    context.mapRect = {0, -.01f, .6f, .3f};
    context.ammoRect = {.6f, .7f, 1.01f, 1};
    CHECK(box.publishContext(context) == epoch && box.snapshot().context.hudValidMask == 0);
}

static void testLastWeaponHasSeparateEdgeAndRevocation() {
    Mailbox box;
    auto context = gameplay();
    const auto epoch = ready(box, context);
    const auto bit = actionBit(Action::LastWeapon);
    CHECK(bit == (std::uint64_t{1} << 25));
    CHECK(send(box, epoch, bit));
    const auto down = box.consumeFrame(2, 101);
    CHECK(down.actionEdgeCount == 1 && down.actionEdges[0].action == Action::LastWeapon && down.actionEdges[0].down);
    context.availableActions &= ~bit;
    CHECK(box.publishContext(context) == epoch);
    const auto up = box.consumeFrame(3, 102);
    CHECK(up.actionEdgeCount == 1 && up.actionEdges[0].action == Action::LastWeapon && !up.actionEdges[0].down);
    CHECK(shippedActionName(Action::LastWeapon) == nullptr);
}

static void testOriginalChatEntryAndRevocation() {
    Mailbox box;
    auto context = gameplay();
    constexpr auto chat = actionBit(Action::Chat);
    constexpr auto fire = actionBit(Action::Fire);
    CHECK(chat == (std::uint64_t{1} << 26) && kActionCount == 31);
    CHECK(std::strcmp(shippedActionName(Action::Chat), "Say") == 0);
    context.availableActions &= ~chat; // A producer without Chat keeps the original 26 actions.
    const auto epoch = ready(box, context);
    CHECK(box.snapshot().wire().size() == 18 && box.snapshot().wire()[0] == kProtocolVersion);
    CHECK(box.submit(epoch, .25f, .75f, 0, 0, fire | chat, 101));
    const auto old = box.consumeFrame(2, 101);
    CHECK(old.actionEdgeCount == 1 && old.actionEdges[0].action == Action::Fire && old.heldActions == fire);
    context.availableActions |= chat;
    CHECK(box.publishContext(context) == epoch && (box.snapshot().wire()[4] & chat) != 0);
    CHECK(box.submit(epoch, .25f, .75f, 0, 0, fire | chat | (std::uint64_t{1} << kActionCount), 102));
    CHECK(box.submit(epoch, .25f, .75f, 0, 0, fire, 102));
    const auto down = box.consumeFrame(3, 102);
    CHECK(down.actionEdgeCount == 1 && down.actionEdges[0].action == Action::Chat && down.actionEdges[0].down);
    CHECK(down.heldActions == (fire | chat) && near(down.moveX, .25f) && near(down.moveY, .75f));
    const auto up = box.consumeFrame(4, 103);
    CHECK(up.actionEdgeCount == 1 && up.actionEdges[0].action == Action::Chat && !up.actionEdges[0].down);
    CHECK(up.heldActions == fire && up.actionEdges[0].sourceEpoch == epoch);
    CHECK(send(box, epoch, fire | chat, 104));
    CHECK(box.consumeFrame(5, 104).heldActions == (fire | chat));
    context.availableActions &= ~chat;
    CHECK(box.publishContext(context) == epoch);
    const auto revoked = box.consumeFrame(6, 105);
    CHECK(revoked.resetReasons == NoReset && revoked.actionEdgeCount == 1 && revoked.heldActions == fire);
    CHECK(revoked.actionEdges[0].action == Action::Chat && !revoked.actionEdges[0].down);
    context.availableActions |= chat;
    CHECK(box.publishContext(context) == epoch);
    CHECK(send(box, epoch, fire | chat, 106));
    box.consumeFrame(7, 106);
    const auto keyboardEpoch = box.publishContext({Mode::TextInput, 0, 0, 2});
    CHECK(keyboardEpoch != epoch && !send(box, epoch, chat, 107));
    const auto focus = box.consumeFrame(8, 107);
    CHECK(focus.resetReasons == ContextChanged && focus.heldActions == 0 && focus.actionEdgeCount == 2);
    for (std::size_t i = 0; i != focus.actionEdgeCount; ++i) CHECK(!focus.actionEdges[i].down);
    CHECK(!send(box, keyboardEpoch, chat, 108));
    context.capabilities |= PhysicalOwnsInput;
    const auto physicalEpoch = box.publishContext(context);
    CHECK(!send(box, physicalEpoch, chat, 109));
}

static void testObjectivesUnknownEmptyAndIndependentValidation() {
    Mailbox box;
    auto context=gameplay();const auto epoch=ready(box,context);
    context.hudValidMask=HudMinimap | HudAmmo | HudObjectives;
    context.mapRect={.7f,.1f,.9f,.3f};context.ammoRect={.8f,.8f,1,1};
    context.objectiveRect={.6f,.35f,.95f,.48f};
    CHECK(box.publishContext(context)==epoch && box.snapshot().wire()[9]==7);
    context.hudValidMask|=HudObjectivesEmpty; // Contradictory producer cannot claim empty.
    CHECK(box.publishContext(context)==epoch);
    CHECK(box.snapshot().context.hudValidMask==(HudMinimap | HudAmmo));
    CHECK(box.snapshot().wire()[14]==0 && box.snapshot().wire()[15]==0);
    context.hudValidMask=HudMinimap | HudAmmo | HudObjectives;
    context.objectiveRect.right=std::numeric_limits<float>::quiet_NaN();
    CHECK(box.publishContext(context)==epoch && box.snapshot().context.hudValidMask==3);
    context.hudValidMask=HudMinimap | HudAmmo | HudObjectivesEmpty;
    CHECK(box.publishContext(context)==epoch && box.snapshot().context.hudValidMask==11);
    CHECK(box.snapshot().wire()[14]==0 && box.snapshot().wire()[15]==0);
    context.hudValidMask=HudObjectives;context.objectiveRect={.6f,.35f,.95f,.48f};
    CHECK(box.publishContext(context)==epoch && box.snapshot().context.hudValidMask==4);
    CHECK(box.snapshot().wire()[10]==0 && box.snapshot().wire()[13]==0);
    context.hudValidMask=0;
    CHECK(box.publishContext(context)==epoch && box.snapshot().wire()[14]==0);
}

static void testUnarmedRevocationKeepsOtherInputs() {
    Mailbox box;auto context=gameplay();const auto epoch=ready(box,context);
    const auto removed=actionBit(Action::Fire) | actionBit(Action::Aim) | actionBit(Action::Reload);
    CHECK(box.submit(epoch,.4f,0,0,0,removed | actionBit(Action::Jump),101));
    box.consumeFrame(2,101);
    context.weaponCount=0;context.knownActions|=removed;context.availableActions&=~removed;
    CHECK(box.publishContext(context)==epoch);
    const auto up=box.consumeFrame(3,102);
    CHECK(up.resetReasons==NoReset && up.actionEdgeCount==3 && up.heldActions==actionBit(Action::Jump));
    CHECK(near(up.moveX,.4f));
    for (std::size_t i=0;i!=up.actionEdgeCount;++i)
        CHECK(!up.actionEdges[i].down && (actionBit(up.actionEdges[i].action)&removed)!=0 && up.actionEdges[i].sourceEpoch==epoch);
}

static void testFastTapSurvivesBetweenTicks() {
    Mailbox box;
    const auto epoch = ready(box);
    const auto fire = actionBit(Action::Fire);
    CHECK(send(box, epoch, fire));
    CHECK(send(box, epoch, 0));
    CHECK(send(box, epoch, fire));
    CHECK(send(box, epoch, 0));
    for (std::uint64_t tick = 2; tick <= 5; ++tick) {
        const auto frame = box.consumeFrame(tick, 101);
        CHECK(frame.actionEdgeCount == 1);
        CHECK(frame.actionEdges[0].action == Action::Fire);
        CHECK(frame.actionEdges[0].down == (tick % 2 == 0));
        CHECK(frame.actionEdges[0].sourceEpoch == epoch);
        CHECK(frame.heldActions == (tick % 2 == 0 ? fire : 0));
    }
    CHECK(box.consumeFrame(6, 101).actionEdgeCount == 0);
}

static void testPauseDebugCapabilityRoundtrip() {
    for (const auto extra : {std::uint32_t(PauseBackdrop),std::uint32_t(DebugCanvas),std::uint32_t(PauseBackdrop | DebugCanvas)}) {
        Mailbox box;
        const auto caps = std::uint32_t(DirectUi | UiNavigation) | extra;
        Context context{Mode::Menu,caps | (1u << 31),actionBit(Action::Confirm) | actionBit(Action::UiDown),42};
        const auto epoch = ready(box,context);
        const auto snapshot = box.snapshot();
        CHECK(snapshot.epoch == epoch && snapshot.context.capabilities == caps && snapshot.wire()[3] == caps);
        CHECK(box.publishContext(context) == epoch); // Unknown bits remain masked; no spurious reset.
        CHECK(send(box,epoch,actionBit(Action::Confirm) | actionBit(Action::UiDown)));
        CHECK(box.uiPointer(epoch,7,PointerPhase::Down,.4f,.3f,101));
        const auto frame = box.consumeFrame(2,101);
        CHECK(frame.context.capabilities == caps && frame.context.availableActions == context.availableActions);
        CHECK(frame.epoch == epoch && frame.actionEdgeCount == 2 && frame.pointerEventCount == 1);
        CHECK(frame.actionEdges[0].action == Action::Confirm && frame.actionEdges[0].down && frame.actionEdges[0].sourceEpoch == epoch);
        CHECK(frame.actionEdges[1].action == Action::UiDown && frame.actionEdges[1].down);
        CHECK(frame.pointerEvents[0].phase == PointerPhase::Down && frame.pointerEvents[0].sourceEpoch == epoch);
        context.capabilities = DirectUi | UiNavigation;
        const auto nextEpoch = box.publishContext(context);
        CHECK(nextEpoch != epoch && !send(box,epoch,0));
        CHECK(!box.uiPointer(epoch,7,PointerPhase::Up,.4f,.3f,102));
        const auto reset = box.consumeFrame(3,102);
        CHECK(reset.epoch == nextEpoch && reset.resetReasons == ContextChanged && reset.actionEdgeCount == 2);
        CHECK(!reset.actionEdges[0].down && reset.actionEdges[0].sourceEpoch == epoch);
        CHECK(!reset.actionEdges[1].down && reset.actionEdges[1].sourceEpoch == epoch);
        CHECK(reset.pointerEventCount == 1 && reset.pointerEvents[0].phase == PointerPhase::Cancel && reset.pointerEvents[0].sourceEpoch == epoch);
        CHECK(send(box,nextEpoch,0,102));
        CHECK(box.consumeFrame(4,102).context.capabilities == context.capabilities);
    }
}

static void testLookOnceAndAnalogHold() {
    Mailbox box;
    const auto epoch = ready(box);
    CHECK(box.submit(epoch, 5, -5, 12, -4, 0, 101));
    CHECK(box.submit(epoch, 5, -5, 7, 2, 0, 102));
    const auto first = box.consumeFrame(2, 102);
    CHECK(near(first.yawDegrees, 19) && near(first.pitchDegrees, -2));
    CHECK(near(first.moveX, std::sqrt(0.5f)) && near(first.moveY, -std::sqrt(0.5f)));
    CHECK(!box.consumeFrame(2, 103).fresh);
    const auto next = box.consumeFrame(3, 104);
    CHECK(near(next.yawDegrees, 0) && near(next.pitchDegrees, 0));
    CHECK(near(first.moveX, next.moveX) && near(first.moveY, next.moveY));
    CHECK(box.snapshot().frameSerial == 3);
    CHECK(box.submit(epoch, 0, 0, 10000, -10000, 0, 105));
    CHECK(box.submit(epoch, 0, 0, 10000, -10000, 0, 105));
    CHECK(box.submit(epoch, 0, 0, 10000, -10000, 0, 105));
    const auto capped = box.consumeFrame(4, 105);
    CHECK(near(capped.yawDegrees, 180) && near(capped.pitchDegrees, -180));
}

static void testResetReleasesAndRearms() {
    Mailbox box;
    const auto oldEpoch = ready(box);
    CHECK(send(box, oldEpoch, actionBit(Action::Fire)));
    CHECK(box.consumeFrame(2, 101).heldActions == actionBit(Action::Fire));
    box.reset();
    const auto epoch = box.snapshot().epoch;
    CHECK(epoch != oldEpoch);
    CHECK(!send(box, oldEpoch, actionBit(Action::Jump)));
    CHECK(!send(box, epoch, actionBit(Action::Jump)));
    CHECK(send(box, epoch, 0));
    CHECK(send(box, epoch, actionBit(Action::Jump)));
    const auto barrier = box.consumeFrame(3, 101);
    CHECK(barrier.resetReasons == ManualReset && barrier.heldActions == 0);
    CHECK(barrier.actionEdgeCount == 1 && !barrier.actionEdges[0].down);
    CHECK(barrier.actionEdges[0].action == Action::Fire);
    CHECK(barrier.actionEdges[0].sourceEpoch == oldEpoch);
    const auto next = box.consumeFrame(4, 101);
    CHECK(next.actionEdgeCount == 1 && next.actionEdges[0].down);
    CHECK(next.actionEdges[0].action == Action::Jump);
    CHECK(next.actionEdges[0].sourceEpoch == epoch);
}

static void testMenuRejectsGameplayAndUnavailableActions() {
    Mailbox box;
    const auto epoch = ready(box, {Mode::Menu, GameplayInput | RelativeLook,
                                  actionBit(Action::Back) | actionBit(Action::Skip), 1});
    CHECK(box.submit(epoch, 1, 1, 30, 30,
                     actionBit(Action::Fire) | actionBit(Action::Back) | actionBit(Action::Skip), 101));
    const auto frame = box.consumeFrame(2, 101);
    CHECK(near(frame.moveX, 0) && near(frame.moveY, 0));
    CHECK(near(frame.yawDegrees, 0) && near(frame.pitchDegrees, 0));
    CHECK(frame.actionEdgeCount == 1 && frame.actionEdges[0].action == Action::Back);
    CHECK(frame.heldActions == actionBit(Action::Back));
}

static void testCinematicSkipAndPauseWithoutAxes() {
    Mailbox box;
    const auto epoch = ready(box, {Mode::Cinematic, CanSkip,
                                  actionBit(Action::Skip) | actionBit(Action::Pause), 1});
    CHECK(box.submit(epoch, 1, 1, 30, 30, actionBit(Action::Fire) | actionBit(Action::Skip), 101));
    const auto held = box.consumeFrame(2, 101);
    CHECK(near(held.moveX, 0) && near(held.moveY, 0));
    CHECK(near(held.yawDegrees, 0) && near(held.pitchDegrees, 0));
    CHECK(held.actionEdgeCount == 1 && held.actionEdges[0].down && held.actionEdges[0].action == Action::Skip);
    CHECK(send(box, epoch, actionBit(Action::Pause), 102));
    const auto swap = box.consumeFrame(3, 102);
    CHECK(swap.actionEdgeCount == 2 && swap.heldActions == actionBit(Action::Pause));
    // An unskippable cinematic keeps Pause but never forwards a held Skip bit.
    const auto pauseOnly = box.publishContext({Mode::Cinematic, CanSkip, actionBit(Action::Pause), 1});
    CHECK(pauseOnly == epoch && send(box, epoch, actionBit(Action::Skip) | actionBit(Action::Pause), 103));
    CHECK(box.consumeFrame(4, 103).heldActions == actionBit(Action::Pause));
    const auto unrouted = box.publishContext({Mode::Cinematic, 0, actionBit(Action::Skip), 1});
    CHECK(unrouted != epoch);
    const auto release = box.consumeFrame(5, 103);
    CHECK(release.resetReasons == ContextChanged && release.heldActions == 0);
    CHECK(box.submit(unrouted, 0, 0, 0, 0, 0, 104) && send(box, unrouted, actionBit(Action::Skip), 104));
    CHECK(box.consumeFrame(6, 104).actionEdgeCount == 0);
}

// Protocol 4: a vehicle seat routes its own shipped actions with axes; on-foot
// actions absent from the seat's availability never reach the engine.
static void testVehicleSeatActionsAndAxes() {
    Mailbox box;
    constexpr auto exit = actionBit(Action::ExitVehicle), seats = actionBit(Action::SwitchSeats);
    constexpr auto turret = actionBit(Action::DriverFireTurret), back = actionBit(Action::LookBack);
    constexpr auto seat = actionBit(Action::Fire) | actionBit(Action::VehicleBoost) | actionBit(Action::VehicleBrake) |
        actionBit(Action::VehicleHorn) | exit | seats | turret | back;
    CHECK(kActionCount == 31 && back == (std::uint64_t{1} << 30) && (kKnownActions & seat) == seat);
    CHECK(std::strcmp(shippedActionName(Action::ExitVehicle), "ExitVehicle") == 0);
    CHECK(std::strcmp(shippedActionName(Action::SwitchSeats), "SwitchSeats") == 0);
    CHECK(std::strcmp(shippedActionName(Action::DriverFireTurret), "DriverFireTurret") == 0);
    CHECK(std::strcmp(shippedActionName(Action::LookBack), "LookBack") == 0);
    const auto epoch = ready(box, {Mode::Vehicle, GameplayInput | RelativeLook, seat, 7});
    CHECK(box.submit(epoch, .6f, .8f, 4, -2, turret | back | actionBit(Action::Jump), 101));
    const auto held = box.consumeFrame(2, 101);
    CHECK(near(held.moveX, .6f) && near(held.moveY, .8f) && near(held.yawDegrees, 4) && near(held.pitchDegrees, -2));
    CHECK(held.heldActions == (turret | back) && held.actionEdgeCount == 2);
    for (std::size_t i = 0; i != held.actionEdgeCount; ++i) CHECK(held.actionEdges[i].down);
    CHECK(send(box, epoch, exit, 102));
    const auto swap = box.consumeFrame(3, 102);
    CHECK(swap.heldActions == exit && swap.actionEdgeCount == 3);
    // Leaving the seat changes the scene: the held Exit is released by the reset.
    const auto onFoot = box.publishContext({Mode::Gameplay, GameplayInput | RelativeLook, kKnownActions, 8});
    CHECK(onFoot != epoch);
    const auto left = box.consumeFrame(4, 103);
    CHECK(left.resetReasons == ContextChanged && left.heldActions == 0 && left.actionEdgeCount == 1);
    CHECK(left.actionEdges[0].action == Action::ExitVehicle && !left.actionEdges[0].down);
}

static void testTextInputRejectsAllTouchCapabilities() {
    Mailbox box;
    const auto oldEpoch = ready(box);
    CHECK(send(box, oldEpoch, actionBit(Action::Fire)));
    CHECK(box.consumeFrame(2, 101).heldActions == actionBit(Action::Fire));
    const auto epoch = box.publishContext({Mode::TextInput,
        GameplayInput | RelativeLook | DirectUi | CanSkip, kKnownActions, 2});
    CHECK(!send(box, epoch, 0));
    CHECK(!box.submit(epoch, 1, 1, 30, 30, actionBit(Action::Confirm), 102));
    CHECK(!box.uiPointer(epoch, 7, PointerPhase::Down, .5f, .5f, 102));
    const auto reset = box.consumeFrame(3, 102);
    CHECK(reset.resetReasons == ContextChanged && reset.actionEdgeCount == 1);
    CHECK(!reset.actionEdges[0].down && reset.actionEdges[0].sourceEpoch == oldEpoch);
    const auto frame = box.consumeFrame(4, 102);
    CHECK(frame.heldActions == 0 && frame.actionEdgeCount == 0 && frame.pointerEventCount == 0);
    CHECK(near(frame.moveX, 0) && near(frame.moveY, 0));
    CHECK(near(frame.yawDegrees, 0) && near(frame.pitchDegrees, 0));
}

static void testSceneIdentityCapabilitiesAndOwnership() {
    Mailbox box;
    auto context = gameplay();
    const auto epoch = ready(box, context);
    CHECK(box.publishContext(context) == epoch);
    CHECK(send(box, epoch, actionBit(Action::Fire)));
    box.consumeFrame(2, 101);
    context.capabilities |= PhysicalOwnsInput;
    const auto physicalEpoch = box.publishContext(context);
    CHECK(physicalEpoch != epoch && !send(box, physicalEpoch, 0));
    const auto barrier = box.consumeFrame(3, 102);
    CHECK(barrier.actionEdgeCount == 1 && !barrier.actionEdges[0].down);
    context.capabilities &= ~PhysicalOwnsInput;
    const auto touchEpoch = box.publishContext(context);
    CHECK(touchEpoch != physicalEpoch && !send(box, touchEpoch, actionBit(Action::Fire)));
    CHECK(send(box, touchEpoch, 0));
    CHECK(box.publishContext(context, true) != touchEpoch);
    const auto forcedEpoch = box.snapshot().epoch;
    ++context.sceneIdentity;
    CHECK(box.publishContext(context) != forcedEpoch);
    const auto sceneEpoch = box.snapshot().epoch;
    context.availableActions &= ~actionBit(Action::Fire);
    CHECK(box.publishContext(context) == sceneEpoch);
    context.mode = Mode::Loading;
    const auto loadingEpoch = box.publishContext(context);
    CHECK(!send(box, loadingEpoch, 0));
    context.mode = Mode::Unknown;
    CHECK(!send(box, box.publishContext(context), 0));
}

static void testActionRevocationDoesNotInterruptMovement() {
    Mailbox box;
    auto context = gameplay();
    const auto epoch = ready(box, context);
    const auto held = actionBit(Action::Fire) | actionBit(Action::Aim);
    CHECK(box.submit(epoch, .4f, .3f, 10, 0, held, 101));
    CHECK(box.consumeFrame(2, 101).heldActions == held);
    CHECK(box.submit(epoch, .5f, .2f, 7, 0, held | actionBit(Action::Jump), 102));
    context.availableActions &= ~(actionBit(Action::Aim) | actionBit(Action::Jump));
    CHECK(box.publishContext(context) == epoch);
    const auto frame = box.consumeFrame(3, 102);
    CHECK(frame.resetReasons == NoReset && frame.heldActions == actionBit(Action::Fire));
    CHECK(near(frame.moveX, .5f) && near(frame.moveY, .2f) && near(frame.yawDegrees, 7));
    CHECK(frame.actionEdgeCount == 1 && frame.actionEdges[0].action == Action::Aim);
    CHECK(!frame.actionEdges[0].down);
    CHECK(box.submit(epoch, .5f, .2f, 0, 0, held, 103));
    CHECK(box.consumeFrame(4, 103).heldActions == actionBit(Action::Fire));
    context.availableActions |= actionBit(Action::Aim);
    CHECK(box.publishContext(context) == epoch);
    CHECK(box.consumeFrame(5, 103).heldActions == actionBit(Action::Fire));
    CHECK(send(box, epoch, held, 104));
    CHECK(box.consumeFrame(6, 104).heldActions == held);
}

static void testActionOverflowReleasesInsteadOfSticking() {
    Mailbox box;
    const auto epoch = ready(box);
    const auto fire = actionBit(Action::Fire);
    CHECK(send(box, epoch, fire));
    box.consumeFrame(2, 101);
    for (std::size_t i = 0; i < kTransitionsPerAction; ++i) {
        CHECK(send(box, epoch, i % 2 == 0 ? 0 : fire));
    }
    CHECK(!send(box, epoch, 0));
    const auto newEpoch = box.snapshot().epoch;
    CHECK(newEpoch != epoch);
    const auto reset = box.consumeFrame(3, 101);
    CHECK(reset.resetReasons == QueueOverflow && reset.heldActions == 0);
    CHECK(reset.actionEdgeCount == 1 && !reset.actionEdges[0].down);
    CHECK(!send(box, newEpoch, fire));
    CHECK(send(box, newEpoch, 0) && send(box, newEpoch, fire));
    CHECK(box.consumeFrame(4, 101).heldActions == fire);
}

static void testPointerOrderingAndSceneCancel() {
    Mailbox box;
    auto context = Context{Mode::Menu, DirectUi, actionBit(Action::Back), 1};
    const auto epoch = ready(box, context);
    CHECK(!box.uiPointer(epoch, 7, PointerPhase::Up, .2f, .3f, 101));
    CHECK(!box.uiPointer(epoch, 7, PointerPhase::Down, -1, 2, 101));
    CHECK(box.uiPointer(epoch, 7, PointerPhase::Down, 0, 1, 101));
    CHECK(box.uiPointer(epoch, 7, PointerPhase::Move, .2f, .3f, 101));
    CHECK(box.uiPointer(epoch, 7, PointerPhase::Move, .4f, .5f, 101));
    CHECK(box.uiPointer(epoch, 7, PointerPhase::Up, .4f, .5f, 101));
    const auto tap = box.consumeFrame(2, 101);
    CHECK(tap.pointerEventCount == 3);
    CHECK(tap.pointerEvents[0].phase == PointerPhase::Down);
    CHECK(near(tap.pointerEvents[0].surfaceX, 0) && near(tap.pointerEvents[0].surfaceY, 1));
    CHECK(tap.pointerEvents[1].phase == PointerPhase::Move);
    CHECK(near(tap.pointerEvents[1].surfaceX, .4f));
    CHECK(tap.pointerEvents[2].phase == PointerPhase::Up);
    CHECK(box.uiPointer(epoch, 7, PointerPhase::Down, .2f, .3f, 102));
    box.consumeFrame(3, 102);
    CHECK(box.uiPointer(epoch, 8, PointerPhase::Down, .8f, .8f, 102));
    ++context.sceneIdentity;
    const auto nextEpoch = box.publishContext(context);
    CHECK(!box.uiPointer(epoch, 7, PointerPhase::Up, .2f, .3f, 103));
    CHECK(send(box, nextEpoch, 0, 103));
    CHECK(box.uiPointer(nextEpoch, 9, PointerPhase::Down, .1f, .1f, 103));
    const auto cancel = box.consumeFrame(4, 103);
    CHECK(cancel.pointerEventCount == 1);
    CHECK(cancel.pointerEvents[0].phase == PointerPhase::Cancel);
    CHECK(cancel.pointerEvents[0].pointerId == 7 && cancel.pointerEvents[0].sourceEpoch == epoch);
    const auto newDown = box.consumeFrame(5, 103);
    CHECK(newDown.pointerEventCount == 1 && newDown.pointerEvents[0].pointerId == 9);
    CHECK(newDown.pointerEvents[0].sourceEpoch == nextEpoch);
}

static void testPointerOverflowCannotBecomeClick() {
    Mailbox box;
    const auto epoch = ready(box, {Mode::Menu, DirectUi, 0, 1});
    CHECK(box.uiPointer(epoch, 1, PointerPhase::Down, .1f, .1f, 101));
    CHECK(box.uiPointer(epoch, 2, PointerPhase::Down, .2f, .2f, 101));
    box.consumeFrame(2, 101);
    for (std::size_t i = 0; i < kPointerQueueCapacity; ++i) {
        CHECK(box.uiPointer(epoch, i % 2 == 0 ? 1 : 2, PointerPhase::Move, .3f, .4f, 101));
    }
    CHECK(!box.uiPointer(epoch, 1, PointerPhase::Up, .3f, .4f, 101));
    const auto reset = box.consumeFrame(3, 101);
    CHECK(reset.resetReasons == QueueOverflow && reset.pointerEventCount == 2);
    CHECK(reset.pointerEvents[0].phase == PointerPhase::Cancel);
    CHECK(reset.pointerEvents[1].phase == PointerPhase::Cancel);
    CHECK(box.consumeFrame(4, 101).pointerEventCount == 0);
}

static void testSurfaceExitCancelsOnlyCapturedPointer() {
    Mailbox box;
    auto context = gameplay();
    context.capabilities |= DirectUi;
    const auto epoch = ready(box, context);
    CHECK(box.submit(epoch, .5f, .2f, 0, 0, actionBit(Action::Fire), 101));
    CHECK(!box.uiPointer(epoch, 5, PointerPhase::Down, 1.01f, .5f, 101));
    CHECK(!box.uiPointer(epoch, 6, PointerPhase::Down, .5f, -.01f, 101));
    CHECK(box.uiPointer(epoch, 7, PointerPhase::Down, .3f, .4f, 101));
    const auto down = box.consumeFrame(2, 101);
    CHECK(down.pointerEventCount == 1 && down.heldActions == actionBit(Action::Fire));
    CHECK(box.uiPointer(epoch, 7, PointerPhase::Move, -.2f, .5f, 102));
    const auto cancelMove = box.consumeFrame(3, 102);
    CHECK(cancelMove.resetReasons == NoReset && box.snapshot().epoch == epoch);
    CHECK(cancelMove.pointerEventCount == 1);
    CHECK(cancelMove.pointerEvents[0].phase == PointerPhase::Cancel);
    CHECK(near(cancelMove.pointerEvents[0].surfaceX, .3f));
    CHECK(near(cancelMove.pointerEvents[0].surfaceY, .4f));
    CHECK(cancelMove.heldActions == actionBit(Action::Fire) && near(cancelMove.moveX, .5f));
    CHECK(!box.uiPointer(epoch, 7, PointerPhase::Up, .3f, .4f, 103));
    CHECK(box.uiPointer(epoch, 9, PointerPhase::Down, .7f, .2f, 103));
    CHECK(box.uiPointer(epoch, 9, PointerPhase::Move, .8f, .3f, 103));
    box.consumeFrame(4, 103);
    CHECK(box.uiPointer(epoch, 9, PointerPhase::Up, 1.2f, .7f, 104));
    const auto cancelUp = box.consumeFrame(5, 104);
    CHECK(cancelUp.pointerEventCount == 1 && cancelUp.resetReasons == NoReset);
    CHECK(cancelUp.pointerEvents[0].phase == PointerPhase::Cancel);
    CHECK(near(cancelUp.pointerEvents[0].surfaceX, .8f));
    CHECK(near(cancelUp.pointerEvents[0].surfaceY, .3f));
    CHECK(cancelUp.heldActions == actionBit(Action::Fire) && box.snapshot().epoch == epoch);
}

static void testShippedLookupSeparatesGameplayFromGfx() {
    CHECK(std::strcmp(shippedActionName(Action::Skill), "ActionSkill") == 0);
    CHECK(std::strcmp(shippedActionName(Action::VehicleBrake), "Handbrake") == 0);
    CHECK(shippedActionName(Action::Back) == nullptr);
}

static void testHeartbeatAndMalformedInput() {
    Mailbox box;
    auto epoch = ready(box);
    CHECK(send(box, epoch, actionBit(Action::Fire)));
    box.consumeFrame(2, 101);
    const auto timeout = box.consumeFrame(3, 101 + kHeartbeatTimeoutMs + 1);
    CHECK(timeout.resetReasons == HeartbeatLost);
    CHECK(timeout.actionEdgeCount == 1 && !timeout.actionEdges[0].down);
    CHECK(!send(box, epoch, actionBit(Action::Fire), 500));
    epoch = box.snapshot().epoch;
    CHECK(send(box, epoch, 0, 500));
    CHECK(send(box, epoch, actionBit(Action::Aim), 500));
    box.consumeFrame(4, 500);
    CHECK(!box.submit(epoch, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0, 500));
    const auto invalid = box.consumeFrame(5, 500);
    CHECK(invalid.resetReasons == InvalidNumeric);
    CHECK(invalid.actionEdgeCount == 1 && invalid.actionEdges[0].action == Action::Aim);
    CHECK(!invalid.actionEdges[0].down && near(invalid.moveX, 0));
    epoch = box.snapshot().epoch;
    CHECK(send(box, epoch, 0, 500));
    CHECK(!box.submit(epoch, 0, 0, std::numeric_limits<float>::infinity(), 0, 0, 500));
    CHECK(box.consumeFrame(6, 500).resetReasons == InvalidNumeric);
}

class RecordingSink final : public EngineSink {
public:
    explicit RecordingSink(Mailbox& mailbox) : box(mailbox) {}
    Mailbox& box;
    unsigned frames = 0;
    unsigned looks = 0;
    std::uint64_t touchActions = 0;
    std::uint64_t physicalActions = actionBit(Action::Fire);
    float right = 0;
    void beginTouchFrame(const Frame&) override {
        ++frames;
        (void)box.snapshot(); // Engine callbacks run outside the mailbox mutex.
    }
    void action(const ActionEdge& edge) override {
        if (edge.down) touchActions |= actionBit(edge.action);
        else touchActions &= ~actionBit(edge.action);
    }
    void pointer(const PointerEvent&) override {}
    void movement(float x, float) override { right = x; }
    void relativeLook(float, float) override { ++looks; }
    void endTouchFrame(const Frame&) override {}
};

static void testConsumerAndSourceOwnership() {
    Mailbox box;
    const auto epoch = ready(box);
    RecordingSink sink(box);
    CHECK(box.submit(epoch, 1, 0, 7, 0, actionBit(Action::Fire), 101));
    CHECK(consumeIntoEngine(box, 2, 101, sink));
    CHECK(sink.touchActions == actionBit(Action::Fire) && near(sink.right, 1));
    CHECK(!consumeIntoEngine(box, 2, 101, sink));
    CHECK(sink.frames == 1 && sink.looks == 1);
    box.reset();
    CHECK(consumeIntoEngine(box, 3, 101, sink));
    CHECK(sink.touchActions == 0 && near(sink.right, 0));
    CHECK(sink.physicalActions == actionBit(Action::Fire));
    CHECK(sink.looks == 1);
}

static void testConcurrentSnapshotIsCoherent() {
    Mailbox box;
    std::atomic<bool> done{false};
    std::atomic<bool> coherent{true};
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= 5000; ++i) {
            const bool odd = (i & 1) != 0;
            box.publishContext({odd ? Mode::Gameplay : Mode::Menu,
                odd ? static_cast<std::uint32_t>(GameplayInput) : static_cast<std::uint32_t>(DirectUi),
                odd ? actionBit(Action::Fire) : actionBit(Action::Back), i});
        }
        done.store(true);
    });
    std::thread reader([&] {
        do {
            const auto snap = box.snapshot();
            if (snap.context.sceneIdentity == 0) continue;
            const bool odd = (snap.context.sceneIdentity & 1) != 0;
            if (snap.context.mode != (odd ? Mode::Gameplay : Mode::Menu) ||
                snap.context.availableActions != (odd ? actionBit(Action::Fire) : actionBit(Action::Back))) {
                coherent.store(false);
            }
        } while (!done.load());
    });
    producer.join();
    reader.join();
    CHECK(coherent.load());
}

static void testUseHudRegionIsIndependent() {
    Mailbox box;
    auto context=gameplay();
    context.knownActions=actionBit(Action::Use);
    context.hudValidMask=HudMinimap | HudUse;
    context.mapRect={.7f,.02f,.9f,.2f};
    context.useRect={.38f,.49f,.65f,.55f};
    const auto epoch=ready(box,context);
    const auto words=box.snapshot().wire();
    CHECK(words.size()==18 && words[0]==kProtocolVersion && words[9]==(HudMinimap|HudUse));
    CHECK(near(unpackFloat(words[16],false),.38f) && near(unpackFloat(words[16],true),.49f));
    CHECK(near(unpackFloat(words[17],false),.65f) && near(unpackFloat(words[17],true),.55f));
    CHECK(send(box,epoch,actionBit(Action::Fire)|actionBit(Action::Use)));
    box.consumeFrame(2,101);
    context.useRect.right=std::numeric_limits<float>::quiet_NaN();
    CHECK(box.publishContext(context)==epoch);
    const auto invalid=box.snapshot();
    CHECK(invalid.context.hudValidMask==HudMinimap && invalid.wire()[16]==0 && invalid.wire()[17]==0);
    CHECK(invalid.context.availableActions&actionBit(Action::Use)); // Genuine availability survives a projection failure.
    context.availableActions&=~actionBit(Action::Use);
    CHECK(box.publishContext(context)==epoch);
    const auto revoked=box.consumeFrame(3,102);
    CHECK(revoked.actionEdgeCount==1 && revoked.actionEdges[0].action==Action::Use && !revoked.actionEdges[0].down);
    CHECK(revoked.heldActions==actionBit(Action::Fire));
}

static void testNativeMapTwoPointerTransport() {
    Mailbox box;
    Context map{Mode::Menu,DirectUi | UiNavigation | MapGestures,actionBit(Action::Back),91};
    const auto epoch=ready(box,map);
    CHECK((box.snapshot().wire()[3]&MapGestures)!=0);
    CHECK(box.uiPointer(epoch,7,PointerPhase::Down,.3f,.5f,101));
    CHECK(box.uiPointer(epoch,42,PointerPhase::Down,.7f,.5f,102));
    const auto down=box.consumeFrame(2,102);
    CHECK(down.pointerEventCount==2 && down.pointerEvents[0].pointerId==7 && down.pointerEvents[1].pointerId==42);
    CHECK(box.uiPointer(epoch,7,PointerPhase::Move,.2f,.5f,103));
    CHECK(box.uiPointer(epoch,42,PointerPhase::Move,.8f,.5f,103));
    const auto pinch=box.consumeFrame(3,103);
    CHECK(pinch.pointerEventCount==2 && near(pinch.pointerEvents[0].surfaceX,.2f) && near(pinch.pointerEvents[1].surfaceX,.8f));
    map.capabilities&=~MapGestures;
    const auto next=box.publishContext(map);
    CHECK(next!=epoch && !box.uiPointer(epoch,42,PointerPhase::Up,.8f,.5f,104));
    const auto cancelled=box.consumeFrame(4,104);
    CHECK(cancelled.pointerEventCount==2 && cancelled.resetReasons==ContextChanged);
    for(std::size_t i=0;i<cancelled.pointerEventCount;++i) CHECK(cancelled.pointerEvents[i].phase==PointerPhase::Cancel);
    CHECK((box.snapshot().wire()[3]&MapGestures)==0);
}

int main() {
    testUnknownAndWire();
    testLiveTelemetryPreservesCaptureAndReleasesRevocation();
    testTelemetryRejectsEachInvalidRectIndependently();
    testLastWeaponHasSeparateEdgeAndRevocation();
    testOriginalChatEntryAndRevocation();
    testObjectivesUnknownEmptyAndIndependentValidation();
    testUnarmedRevocationKeepsOtherInputs();
    testFastTapSurvivesBetweenTicks();
    testPauseDebugCapabilityRoundtrip();
    testLookOnceAndAnalogHold();
    testResetReleasesAndRearms();
    testMenuRejectsGameplayAndUnavailableActions();
    testCinematicSkipAndPauseWithoutAxes();
    testVehicleSeatActionsAndAxes();
    testTextInputRejectsAllTouchCapabilities();
    testSceneIdentityCapabilitiesAndOwnership();
    testActionRevocationDoesNotInterruptMovement();
    testActionOverflowReleasesInsteadOfSticking();
    testPointerOrderingAndSceneCancel();
    testSurfaceExitCancelsOnlyCapturedPointer();
    testShippedLookupSeparatesGameplayFromGfx();
    testPointerOverflowCannotBecomeClick();
    testHeartbeatAndMalformedInput();
    testConsumerAndSourceOwnership();
    testConcurrentSnapshotIsCoherent();
    testNativeMapTwoPointerTransport();
    testUseHudRegionIsIndependent();
    std::cout << "PASS: 27 native touch contract scenarios\n";
    return 0;
}

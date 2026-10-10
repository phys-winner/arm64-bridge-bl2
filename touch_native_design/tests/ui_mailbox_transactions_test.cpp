#include "bl2_touch_input.h"
#include "bl2_touch_ui_transactions.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace bl2::touch;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL " << __func__ << ':' << __LINE__ << ": " #condition << '\n'; \
    std::exit(1); } } while (false)

// Exercise the actual producer queue and actual UI transaction state together.
// This models Ue3UiSink's reset/capability gate, without an Android or guest stub.
struct Harness {
    Mailbox mailbox;
    UiTransactions ui;
    Context context{Mode::Menu, UiNavigation, kUiActions, 42};
    std::uint64_t epoch = 0;
    std::uint64_t tick = 0;
    std::vector<Action> commands;

    Harness() {
        epoch = mailbox.publishContext(context);
        CHECK(send(0));
        CHECK(drain().resetReasons == ContextChanged);
    }
    bool send(std::uint64_t held, std::uint64_t now = 100) {
        return mailbox.submit(epoch, 0, 0, 0, 0, held, now);
    }
    void tap(Action action) {
        CHECK(send(actionBit(action)));
        CHECK(send(0));
    }
    Frame drain(std::uint64_t now = 100) {
        const auto frame = mailbox.consumeFrame(++tick, now);
        CHECK(frame.fresh);
        CHECK(frame.actionEdgeCount <= frame.actionEdges.size());
        const bool valid = (frame.context.capabilities & UiNavigation) != 0 &&
            (frame.context.capabilities & PhysicalOwnsInput) == 0;
        ui.begin(frame.epoch, frame.context.sceneIdentity, valid,
                 frame.resetReasons != NoReset, 2424, 1080);
        for (std::size_t i = 0; i < frame.actionEdgeCount; ++i) {
            if (const auto command = ui.action(frame.actionEdges[i])) commands.push_back(*command);
        }
        return frame;
    }
};

static void testDifferentSequentialTaps() {
    Harness h;
    h.tap(Action::Confirm);
    h.tap(Action::UiDown);
    const auto first = h.drain();
    const auto second = h.drain();
    std::cout << "different-button taps: first_edges=" << first.actionEdgeCount
              << " second_edges=" << second.actionEdgeCount
              << " committed=" << h.commands.size() << '\n';
    CHECK((h.commands == std::vector<Action>{Action::Confirm, Action::UiDown}));
    CHECK(first.actionEdgeCount == 4 && second.actionEdgeCount == 0);
    for (std::size_t i = 0; i < first.actionEdgeCount; ++i)
        CHECK(first.actionEdges[i].sequence == i + 1);
    CHECK(first.heldActions == 0);
}

static void testReverseEnumAndSameButtonOrder() {
    Harness h;
    const std::vector<Action> expected{Action::UiRight, Action::Back, Action::Confirm,
                                       Action::Confirm, Action::UiLeft};
    for (const auto action : expected) h.tap(action);
    CHECK(h.drain().actionEdgeCount == expected.size() * 2);
    CHECK(h.commands == expected);
    CHECK(h.drain().actionEdgeCount == 0);
}

static void testActualOverlapStillCancels() {
    Harness h;
    CHECK(h.send(actionBit(Action::Confirm)));
    CHECK(h.send(actionBit(Action::Confirm) | actionBit(Action::UiDown)));
    CHECK(h.send(actionBit(Action::UiDown)));
    CHECK(h.send(0));
    CHECK(h.drain().actionEdgeCount == 4);
    CHECK(h.commands.empty());
    h.tap(Action::Back);
    h.drain();
    CHECK((h.commands == std::vector<Action>{Action::Back}));
}

static void testFrameCapacityRetainsTail() {
    Harness h;
    std::vector<Action> expected;
    // Exactly fill two per-action rings (16 edges each), exceeding Frame's 25.
    for (std::size_t i = 0; i < kTransitionsPerAction; ++i) {
        const auto action = i % 2 == 0 ? Action::Confirm : Action::UiDown;
        h.tap(action);
        expected.push_back(action);
    }
    const auto first = h.drain();
    CHECK(first.actionEdges.size() >= 25 && first.actionEdgeCount == 25);
    CHECK(first.heldActions == actionBit(Action::Confirm)); // Edge 25 is DOWN.
    CHECK(h.commands.size() == 12);
    CHECK(!h.mailbox.consumeFrame(h.tick, 100).fresh);
    const auto second = h.drain();
    CHECK(second.actionEdgeCount == 7 && second.heldActions == 0);
    std::uint64_t sequence = 0;
    for (const auto* frame : {&first, &second}) {
        CHECK(frame->epoch == h.epoch && frame->resetReasons == NoReset);
        for (std::size_t i = 0; i < frame->actionEdgeCount; ++i) {
            CHECK(frame->actionEdges[i].sequence == ++sequence);
            CHECK(frame->actionEdges[i].sourceEpoch == h.epoch);
        }
    }
    CHECK(sequence == 32 && h.commands == expected);
    CHECK(h.drain().actionEdgeCount == 0);
}

static void testResetAndNewEpochBarrier() {
    Harness h;
    const auto oldEpoch = h.epoch;
    CHECK(h.send(actionBit(Action::Back)));
    CHECK(h.drain().heldActions == actionBit(Action::Back));
    CHECK(h.commands.empty());
    CHECK(h.send(0));
    h.tap(Action::Confirm);
    h.mailbox.reset();
    h.epoch = h.mailbox.snapshot().epoch;
    CHECK(h.epoch != oldEpoch);
    CHECK(!h.mailbox.submit(oldEpoch, 0, 0, 0, 0, 0, 100));
    CHECK(!h.send(actionBit(Action::UiRight)));
    CHECK(h.send(0));
    h.tap(Action::UiRight); // Must survive the neutral barrier, not join old UP.
    const auto barrier = h.drain();
    CHECK(barrier.resetReasons == ManualReset && barrier.actionEdgeCount == 1);
    CHECK(!barrier.actionEdges[0].down && barrier.actionEdges[0].sourceEpoch == oldEpoch);
    CHECK(h.commands.empty());
    CHECK(h.drain().actionEdgeCount == 2);
    CHECK((h.commands == std::vector<Action>{Action::UiRight}));
}

static void testSceneChangeAndPhysicalTakeover() {
    for (const bool physical : {false, true}) {
        Harness h;
        const auto oldEpoch = h.epoch;
        CHECK(h.send(actionBit(Action::Back)));
        h.drain();
        CHECK(h.send(0));
        h.tap(Action::Confirm);
        if (physical) h.context.capabilities |= PhysicalOwnsInput;
        else ++h.context.sceneIdentity;
        h.epoch = h.mailbox.publishContext(h.context);
        CHECK(h.epoch != oldEpoch);
        CHECK(!h.mailbox.submit(oldEpoch, 0, 0, 0, 0, 0, 100));
        if (physical) CHECK(!h.send(0));
        const auto barrier = h.drain();
        CHECK(barrier.resetReasons == ContextChanged && barrier.actionEdgeCount == 1);
        CHECK(!barrier.actionEdges[0].down && barrier.actionEdges[0].sourceEpoch == oldEpoch);
        CHECK(h.commands.empty());
        CHECK(h.drain().actionEdgeCount == 0);
        if (physical) {
            h.context.capabilities &= ~PhysicalOwnsInput;
            h.epoch = h.mailbox.publishContext(h.context);
            CHECK(h.drain().resetReasons == ContextChanged);
        }
        CHECK(!h.send(actionBit(Action::UiRight)));
        CHECK(h.send(0));
        h.tap(Action::UiRight);
        CHECK(h.drain().actionEdgeCount == 2);
        CHECK((h.commands == std::vector<Action>{Action::UiRight}));
    }
}

static void testResetWhileCapacityTailPending() {
    Harness h;
    for (std::size_t i = 0; i < kTransitionsPerAction; ++i)
        h.tap(i % 2 == 0 ? Action::Confirm : Action::UiDown);
    CHECK(h.drain().actionEdgeCount == 25);
    CHECK(h.commands.size() == 12);
    h.mailbox.reset();
    const auto reset = h.drain();
    CHECK(reset.resetReasons == ManualReset && reset.actionEdgeCount == 1);
    CHECK(h.commands.size() == 12); // Pending DOWN 25 and remaining tail cancelled.
    CHECK(h.drain().actionEdgeCount == 0);
}

static void testHeartbeatAndQueueOverflowCancel() {
    for (const bool overflow : {false, true}) {
        Harness h;
        CHECK(h.send(actionBit(Action::Back)));
        h.drain();
        Frame reset;
        if (overflow) {
            for (std::size_t i = 0; i < kTransitionsPerAction; ++i)
                CHECK(h.send(i % 2 == 0 ? 0 : actionBit(Action::Back)));
            CHECK(!h.send(0));
            reset = h.drain();
            CHECK(reset.resetReasons == QueueOverflow);
        } else {
            reset = h.drain(100 + kHeartbeatTimeoutMs + 1);
            CHECK(reset.resetReasons == HeartbeatLost);
        }
        CHECK(reset.actionEdgeCount == 1 && !reset.actionEdges[0].down);
        CHECK(h.commands.empty());
    }
}

static void testGameplayKeepsOneEdgePerActionPerTick() {
    for (const auto extra : {0u, static_cast<unsigned>(UiNavigation)}) {
        Mailbox box;
        const auto epoch = box.publishContext({Mode::Gameplay,
            GameplayInput | RelativeLook | extra, kKnownActions, 77});
        CHECK(box.submit(epoch, 0, 0, 0, 0, 0, 100));
        CHECK(box.consumeFrame(1, 100).resetReasons == ContextChanged);
        for (unsigned i = 0; i < 4; ++i)
            CHECK(box.submit(epoch, .5f, 0, i == 0 ? 5.0f : 0.0f, 0,
                             i % 2 == 0 ? actionBit(Action::Fire) : 0, 100));
        for (std::uint64_t tick = 2; tick <= 5; ++tick) {
            const auto frame = box.consumeFrame(tick, 100);
            CHECK(frame.actionEdgeCount == 1 && frame.actionEdges[0].action == Action::Fire);
            CHECK(frame.actionEdges[0].down == (tick % 2 == 0));
            CHECK(frame.heldActions == (tick % 2 == 0 ? actionBit(Action::Fire) : 0));
            CHECK(frame.moveX == .5f && frame.yawDegrees == (tick == 2 ? 5.0f : 0.0f));
        }
    }
}

int main() {
    testDifferentSequentialTaps();
    testReverseEnumAndSameButtonOrder();
    testActualOverlapStillCancels();
    testFrameCapacityRetainsTail();
    testResetAndNewEpochBarrier();
    testSceneChangeAndPhysicalTakeover();
    testResetWhileCapacityTailPending();
    testHeartbeatAndQueueOverflowCancel();
    testGameplayKeepsOneEdgePerActionPerTick();
    std::cout << "PASS: 9 Mailbox + UiTransactions scenarios; ordered UI taps, bounded tail, "
                 "reset/scene/physical/heartbeat/overflow cancellation, gameplay cadence unchanged\n";
}

#include "bl2_touch_physical.h"
#include "../arm64_bridge/bl2_touch_input.h"
#include <array>
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    using bl2::touch::PhysicalOwnership;
    {
        PhysicalOwnership state;
        assert(!state.ownsInput(1));
        state.key(0, 0, 96, 100);
        assert(state.ownsInput(100000)); // A held physical action never times out.
        state.key(0, 1, 96, 100000);
        assert(state.ownsInput(100799));
        assert(!state.ownsInput(100800));
    }
    {
        ResetBl2TouchPhysicalOwnership();
        BeginBl2TouchLegacyPacket(); BeginBl2TouchLegacyPacket();
        NotifyBl2TouchPhysicalKey(1, 0, 96);
        EndBl2TouchLegacyPacket(); EndBl2TouchLegacyPacket();
        assert(!bl2::touch::physicalOwnership().ownsInput(bl2::touch::monotonicMilliseconds() + 1000));
        NotifyBl2TouchPhysicalKey(1, 0, 96);
        assert(bl2::touch::physicalOwnership().ownsInput(bl2::touch::monotonicMilliseconds() + 1000));
        NotifyBl2TouchPhysicalKey(1, 1, 96);
    }
    {
        PhysicalOwnership state;
        state.key(0, 0, 96, 10); state.key(1, 0, 96, 10);
        state.key(0, 1, 96, 20);
        assert(state.ownsInput(9000)); // Same key, distinct controller ownership.
        state.key(1, 1, 96, 9010);
        assert(!state.ownsInput(9810));
        state.key(0, 0, 96, 9900); state.key(1, 0, 96, 9900);
        state.disconnected(0, 10000);
        assert(state.ownsInput(20000));
        state.disconnected(1, 20000);
        assert(!state.ownsInput(20800));
    }
    {
        PhysicalOwnership state;
        std::array<float, 8> axes{};
        state.motion(0, axes, 10);
        assert(!state.ownsInput(10)); // Repeated idle samples must not starve touch.
        axes[2] = .4f; state.motion(0, axes, 11);
        assert(state.ownsInput(100000));
        axes[2] = 0; state.motion(0, axes, 100000);
        assert(state.ownsInput(100799)); assert(!state.ownsInput(100800));
        axes[4] = std::numeric_limits<float>::quiet_NaN(); state.motion(0, axes, 200000);
        assert(state.ownsInput(300000)); // Bad producer data fails closed.
    }
    {
        PhysicalOwnership state;
        state.mouse(2, 1, 0, 0, 10); state.mouse(2, 0, 1, 0, 20);
        state.mouse(2, 2, 0, 0, 30);
        assert(state.ownsInput(10000));
        state.mouse(2, 0, 2, 0, 10000);
        assert(!state.ownsInput(10800));
        state.mouse(2, 0, 0, 1, 10801);
        assert(state.ownsInput(10801));
    }
    {
        PhysicalOwnership state;
        state.key(0, 99, 1, 10);
        assert(state.ownsInput(100000));
        state.reset(100000);
        assert(state.ownsInput(99999)); // Clock rollback cannot remove ownership.
        assert(!state.ownsInput(100800));
    }
    {
        PhysicalOwnership state;
        state.disconnected(0, 50000);
        assert(!state.ownsInput(50001)); // A device change with nothing held is not input.
        state.key(0, 0, 96, 50002);
        state.disconnected(0, 60000);
        assert(state.ownsInput(60799) && !state.ownsInput(60800)); // A held key keeps the neutral interval.
    }
    {
        // A keyboard (controller 0: letters, arrows) never owns input: the captured
        // mouse and touch keep their route beside it; it is only reported.
        PhysicalOwnership state;
        assert(PhysicalOwnership::keyboardKey(0, 51) && PhysicalOwnership::keyboardKey(0, 19) &&
               !PhysicalOwnership::keyboardKey(0, 96) && !PhysicalOwnership::keyboardKey(0, 110) &&
               !PhysicalOwnership::keyboardKey(0, 188) && !PhysicalOwnership::keyboardKey(1, 51));
        state.key(0, 0, 51, 1000); // W held
        assert(!state.ownsInput(100000) && state.keyboardActive(100000) && state.anyPhysical(100000));
        state.key(0, 1, 51, 100000);
        assert(state.keyboardActive(100799) && !state.keyboardActive(100800) && !state.anyPhysical(100800));
        state.key(1, 0, 96, 200000); // A gamepad still takes over.
        assert(state.ownsInput(200000) && state.anyPhysical(200000) && !state.keyboardActive(200900));
        state.key(1, 1, 96, 200000);
        state.key(0, 0, 32, 300000); state.disconnected(0, 300500); // pointer capture reconfigures the mouse
        assert(!state.ownsInput(300500) && state.keyboardActive(301299) && !state.keyboardActive(301300));
        state.key(0, 7, 32, 400000);
        assert(state.ownsInput(500000)); // An unknown key action still fails closed.
    }
    {
        assert(!bl2::touch::TouchDispatchScope::active());
        {
            const bl2::touch::TouchDispatchScope outer;
            { const bl2::touch::TouchDispatchScope inner; assert(bl2::touch::TouchDispatchScope::active()); }
            assert(bl2::touch::TouchDispatchScope::active());
        }
        assert(!bl2::touch::TouchDispatchScope::active());
    }
    std::cout << "PASS: physical held-key, controller, axis, mouse, neutral and malformed-input ownership; "
                 "keyboard beside mouse and touch; touch dispatch scope\n";
}

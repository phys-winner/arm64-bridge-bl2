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
    std::cout << "PASS: physical held-key, controller, axis, mouse, neutral and malformed-input ownership\n";
}

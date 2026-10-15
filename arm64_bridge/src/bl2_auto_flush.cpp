// Submit long frames in parts on the drawing thread.
//
// Measured on Adreno 650 (abb24fae, Outwash): the driver keeps a whole frame of
// ~1650 draws unsubmitted, the GPU idles until the guest's own glFlush or the
// swap, and near the end of the frame one draw blocks 10-17 ms in
// "FreeAllocationOnTimestamp - WaitForTimestamp" until the GPU catches up.
// A glFlush after every 500-1000 draws removed those waits (static recompiler
// build: 20-22 -> 32-33 FPS) while costing ~0.2 ms per flush.
//
// The count restarts at every guest glFlush/glFinish and swap, so frames that
// already submit often are untouched. debug.bl2.auto_flush=N (read once before
// launch) sets the draw count; 0 disables the feature.
#include <GLES3/gl32.h>
#include <sys/system_properties.h>

#include <cstdlib>

namespace {
constexpr unsigned kDefaultDraws = 600;

unsigned ConfiguredDraws() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2.auto_flush", value) <= 0) return kDefaultDraws;
    char* end = nullptr;
    const unsigned long draws = std::strtoul(value, &end, 10);
    return end != value && draws <= 100000 ? static_cast<unsigned>(draws) : kDefaultDraws;
}

const unsigned g_draws_per_flush = ConfiguredDraws();
thread_local unsigned t_draws_since_flush = 0;
}  // namespace

void Bl2AutoFlushNoteDraw() {
    if (!g_draws_per_flush) return;
    if (++t_draws_since_flush < g_draws_per_flush) return;
    t_draws_since_flush = 0;
    glFlush();
}

void Bl2AutoFlushReset() {
    t_draws_since_flush = 0;
}

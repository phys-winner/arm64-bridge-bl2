#pragma once

#include <EGL/egl.h>

#include <cstdint>

// Present pacing for the guest's window surface. Unpaced, each frame is shown
// at the first refresh after the GPU finishes it, so below the display rate a
// 120 Hz panel holds frames for 2, 3 and 4 refreshes in turn: the FPS counter
// stays steady while motion judders. The pacer gives every frame the same whole
// number of refreshes (EGL_ANDROID_presentation_time), chooses that interval
// from measured render-thread and GPU times, and holds the render thread until
// the next frame is due, which also gives the guest game thread (one frame
// behind it) the same cadence. The FRAME PACING setting that switched it while
// playing is commented out by user request, so the bridge passes allowed=false
// (bl2_touch_settings.h Bl2FramePacingEnabled); debug.bl2.frame_pacing=0 before
// launch also disables it.

struct Bl2FramePace {
    float paced_fps;  // Presented rate, 0 while frames are unpaced.
    // CPU hint target: the next faster pace, or the shown one while the GPU
    // limits the pace. 0 if unpaced.
    float aim_fps;
};

// Render thread, every present, before Bl2FramePacerBeforeSwap: the active
// display rate and the user's limit (0 = display rate).
Bl2FramePace Bl2FramePacerConfigure(float display_hz, uint32_t limit_fps, bool allowed);
// True once the pacer drives a window surface (any thread).
bool Bl2FramePacerActive();
// Render thread, immediately before eglSwapBuffers.
void Bl2FramePacerBeforeSwap(EGLDisplay display, EGLSurface surface);
// Render thread, after a successful eglSwapBuffers: the CLOCK_MONOTONIC time
// at which the next frame should start, or 0 when it may start now.
uint64_t Bl2FramePacerAfterSwap();
// Render thread, after its idle work: sleeps until `start_ns` (0 = no wait).
void Bl2FramePacerWait(uint64_t start_ns);
// eglDestroySurface, before the surface is destroyed.
void Bl2FramePacerSurfaceDestroyed(EGLSurface surface);

struct Bl2FramePacerStats {
    uint64_t interval_ns;  // 0 while unpaced.
    uint64_t depth_ns;     // Frame start to its present.
    uint32_t late;         // Frames shown after their refresh, since the last call.
    uint32_t changes;      // Interval/depth changes, since the last call.
};
// Render thread, for the hitch summary.
Bl2FramePacerStats Bl2FramePacerTakeStats();

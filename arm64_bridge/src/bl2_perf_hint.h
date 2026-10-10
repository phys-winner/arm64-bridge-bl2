#pragma once

#include <sys/types.h>

// Android Performance Hint (ADPF) session for the guest render and game
// threads. debug.bl2.perf_hint=0 before launch disables it.
#if defined(BL2_NATIVE_ARM32)
inline void Bl2PerfHintFrame(float) {}
inline void Bl2PerfHintGameThread() {}
#else
// Once per presented frame on the render thread with the active tick limit.
// The first caller owns the session; calls from any other thread are ignored.
void Bl2PerfHintFrame(float frame_limit_fps);
// Each guest game tick; records the calling thread once.
void Bl2PerfHintGameThread();
// The recorded game thread, or zero before its first tick.
pid_t Bl2PerfHintGameTid();
#endif

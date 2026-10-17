#pragma once

#include <cstdint>

// Frame black box: cheap counters of the work that can stall a frame, logged
// (tag BL2Hitch) only for a frame longer than twice the active frame budget,
// plus a short summary every ~10 s. debug.bl2.hitch_log=0 disables the log.
#if defined(BL2_NATIVE_ARM32)
inline void Bl2HitchCountJitBlock() {}
inline void Bl2HitchAddPipelineNs(uint64_t) {}
inline void Bl2HitchAddStageNs(uint64_t, bool) {}
inline void Bl2HitchAddTextureUpload(uint64_t) {}
inline void Bl2HitchAddBufferUpload(uint64_t) {}
inline void Bl2HitchCountDraw() {}
inline void Bl2HitchCountProgramSwap() {}
inline void Bl2HitchSetFrameLimit(float) {}
inline void Bl2HitchFrame() {}
inline uint64_t Bl2HitchFrameIndex() { return 0; }
#else
void Bl2HitchCountJitBlock();                 // Any thread, one new JIT block.
void Bl2HitchAddPipelineNs(uint64_t ns);      // Draw-time pair/pipeline resolution miss.
// A guest shader stage created (glNamedProgramStringEXT); compiled = the
// driver compiled it, rather than a cached binary import.
void Bl2HitchAddStageNs(uint64_t ns, bool compiled);
void Bl2HitchAddTextureUpload(uint64_t bytes);
void Bl2HitchAddBufferUpload(uint64_t bytes);
void Bl2HitchCountDraw();
void Bl2HitchCountProgramSwap();
void Bl2HitchSetFrameLimit(float fps);        // Present thread, before the swap.
void Bl2HitchFrame();                         // Present thread, after the swap.
uint64_t Bl2HitchFrameIndex();                // Presented frames so far.
#endif

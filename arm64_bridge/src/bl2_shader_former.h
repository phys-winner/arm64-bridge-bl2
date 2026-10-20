#pragma once

#include "bl2_shader_stage_cache.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <cstddef>
#include <cstdint>

// Shader formation in the background. A first launch used to compile all
// 38,980 catalogue stages before its first frame (16.7 minutes on a Pixel 10a,
// Mali-G715; 21.6 on a Redmi Note 8 Pro, Mali-G76). Now worker threads, each
// with its own unshared context on the game's display, compile the stages the
// game creates first (the shipped first-use list) and then the rest of the
// catalogue, persisting every binary. The renderer imports a ready binary or compiles a missing stage
// itself; a complete pass commits the marker, so later launches skip formation.
// Unshared contexts take no share-group lock of the renderer: render frame
// times were unchanged with compile workers on both test GPUs.
// debug.bl2.shader_former before launch: 0 = no formation (stages compile when
// the game creates them), 1..6 = worker count, full = the old blocking pass.

struct Bl2ShaderFormerAsset {
    void* opaque = nullptr;
    uint64_t bytes = 0;
    size_t (*read)(void*, uint8_t*, size_t) = nullptr;
    void (*close)(void*) = nullptr;
};
// Opens a fresh sequential reader of a packaged asset; false when it is absent.
using Bl2ShaderFormerOpen = bool (*)(const char* name, Bl2ShaderFormerAsset* asset);

enum class Bl2ShaderFormerMode { Background, OnDemand, Blocking };
// The configured mode and worker count (read once per process).
Bl2ShaderFormerMode Bl2ShaderFormerConfiguredMode();
unsigned Bl2ShaderFormerConfiguredThreads();

// Starts formation once per process for an opened, incomplete cache. The cache
// outlives the process; `display` is the renderer's display.
bool Bl2ShaderFormerStart(const Bl2ShaderStageCache* cache, EGLDisplay display,
                          Bl2ShaderFormerOpen open, unsigned threads);
// Activity lifecycle: workers finish their current stage, then wait.
void Bl2ShaderFormerPause(bool paused);
// Formation is running (started and not finished).
bool Bl2ShaderFormerActive();
// While formation runs (any thread, cheap): catalogue stages with a binary on
// disk, counted once at its start plus those formed or compiled on demand
// since, out of `total`. False before that count and once formation stopped.
bool Bl2ShaderFormerProgress(uint32_t* ready, uint32_t* total);
// True while the player is in the world (not a menu, dialog or loading screen):
// catalogue work then moves to the little cores and yields to renderer misses.
using Bl2ShaderFormerGameplayProbe = bool (*)();
void Bl2ShaderFormerSetGameplayProbe(Bl2ShaderFormerGameplayProbe probe);
// The renderer compiled a missing stage itself (any thread, cheap): catalogue
// work pauses for a moment, and a level-specific stage makes the former form
// the rest of its package next. `digest`: SHA-256 of the original source.
void Bl2ShaderFormerNoteDemandCompile(GLenum target, const uint8_t* digest);
// The game started reading cooked package `name` (e.g. "Glacial_P.xxx"): a
// level package is formed next, during its loading screen.
void Bl2ShaderFormerHintPackage(const char* name);

// First-use list asset: magic "BL2SFIR1", version 1, record count, payload
// bytes, payload SHA-256, then catalogue records (GLES target, source bytes,
// source SHA-256, original normalized GLSL) in the order the game creates them.
inline constexpr std::array<uint8_t, 8> kBl2ShaderFirstMagic{'B','L','2','S','F','I','R','1'};

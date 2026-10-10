#pragma once

#include "bl2_shader_stage_cache.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Mali validates a separable program pipeline on every draw (interface symbol
// matching with string formatting), about 7% of the render thread in combat.
// One linked program skips it. A worker with its own context in the renderer's
// share group links hot vertex/fragment pairs off the render thread, or imports
// their cached binaries; the renderer swaps finished programs in between draws.
// debug.bl2.mono_programs=0 before launch keeps every pair on its SSO pipeline.

struct Bl2ProgramUpgradeRequest {
    EGLContext context = EGL_NO_CONTEXT;  // Context owning the SSO pipeline.
    uint64_t key = 0;                     // Renderer pair key.
    uint64_t serial = 0;                  // Pipeline identity at request time.
    std::string vertex_source, fragment_source;  // Original normalized GLSL.
    ObbSha256::Digest vertex_digest{}, fragment_digest{};
};

struct Bl2ProgramUniformArray { GLint location = -1; GLsizei size = 0; };

// Arrays in renderer order: VConstGlobal, VConstFloat, VConstBones, VConstInt,
// VConstBool, PConstGlobal, PConstFloat, PConstInt, PConstBool.
struct Bl2ProgramUpgradeResult {
    EGLContext context = EGL_NO_CONTEXT;
    uint64_t key = 0, serial = 0;
    GLuint program = 0;  // Zero: the pair keeps its pipeline.
    bool imported = false;
    std::array<Bl2ProgramUniformArray, 9> arrays{};
};

bool Bl2ProgramUpgradeEnabled();
// Renderer thread with a context of `group` current. The first submission
// starts the worker for that share group; another group is refused.
bool Bl2ProgramUpgradeSubmit(uint64_t group, Bl2ProgramUpgradeRequest request,
                             const Bl2ShaderStageCache* cache);
bool Bl2ProgramUpgradeHasResults();
// Finished results for `context` only, at most `limit`.
std::vector<Bl2ProgramUpgradeResult> Bl2ProgramUpgradeTake(EGLContext context,
                                                           size_t limit);
uint64_t Bl2ProgramUpgradeGroup();
// The share group lost its last renderer context: stop and join the worker.
// Its context is destroyed with every program not yet handed over.
void Bl2ProgramUpgradeStop();

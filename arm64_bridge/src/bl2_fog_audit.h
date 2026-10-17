#pragma once
#include <GLES3/gl32.h>
#include <cstdint>

// Opt-in, bounded probes. Mode 1 logs fog state; modes 2/3 read pixels and
// can perturb GPU timing. All modes are disabled by default.
bool Bl2FogAuditActive();
void Bl2FogAuditDraw(bool fog, const char* operation, GLint first, GLsizei count,
                    GLenum type, uint32_t indices, GLint base, GLsizei instances);
void Bl2FogAuditCompleted();
void Bl2FogAuditPresent();
bool Bl2FinalAuditActive();
void Bl2FinalAuditDraw();

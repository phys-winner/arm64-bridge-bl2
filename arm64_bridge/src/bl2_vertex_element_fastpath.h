#pragma once

class DynarmicHost;

// Optional ARM64 implementations of BL2's guest
// FOpenGLDynamicRHI::EnableVertexElement. Modes 1 and 2 are diagnostic;
// unusual cases resume the original ARM code.
bool InstallBl2VertexElementFastpath(DynarmicHost& host);

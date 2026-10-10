#pragma once

class DynarmicHost;

// Optional ARM64 implementations of BL2's guest
// FOpenGLDynamicRHI::EnableVertexElement. Modes 1 and 2 are diagnostic;
// unusual cases resume the original ARM code.
bool InstallBl2VertexElementFastpath(DynarmicHost& host);

// Whole SetupVertexArrays candidate. Installed only for startupEnabled or an
// explicit debug.bl2.setup_arrays=1. Property 0 disables installation; invalid
// values keep it off. A disabled/unusual call executes the saved original ARM
// entry. The runtime setter has no effect without startup installation.
bool InstallBl2SetupVertexArraysFastpath(DynarmicHost& host,
                                        bool startupEnabled = false);
void SetBl2SetupVertexArraysEnabled(bool enabled);

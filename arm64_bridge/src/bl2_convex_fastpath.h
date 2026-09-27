#pragma once

class DynarmicHost;

// Optional ARM64 NEON implementation of the guest frustum/box intersection.
// The original ARM32 function is retained unless a differential self-test passes.
bool InstallBl2ConvexFastpath(DynarmicHost& host);

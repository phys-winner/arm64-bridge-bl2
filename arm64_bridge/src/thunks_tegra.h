#pragma once

class ThunkManager;
class DynarmicHost;

// Explicit fail-closed imports for the Tegra Secure Interface runtime. The
// bridge does not pretend a Qualcomm/Pixel has /dev/nvhost-tsec; each call is
// logged and returns an unavailable result, with allocation returning NULL.
void RegisterUnavailableTegraThunks(ThunkManager& manager);

// Borderlands 2's local NVSI lifecycle adapter. This removes the unavailable
// TSEC/channel startup dependency, not the gameplay snippet dependency. The
// latter remains an explicit porting/acceptance debt; no snippet returns fake
// successful output. Only audited, signature-matched guest entries are patched.
bool InstallBorderlandsNvsiLifecycle(DynarmicHost& host);

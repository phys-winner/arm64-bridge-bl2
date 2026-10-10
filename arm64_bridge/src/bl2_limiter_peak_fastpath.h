#pragma once

class DynarmicHost;

// Startup-only opt-in for the pinned limiter peak loop. Defaults to disabled.
bool InstallBl2LimiterPeakFastpath(DynarmicHost& host, bool startupEnabled = false);
void SetBl2LimiterPeakEnabled(bool enabled);

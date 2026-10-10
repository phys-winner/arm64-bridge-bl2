#pragma once
class DynarmicHost;
// Exact pinned guest constant-state update; experimental opt-in until measured.
bool InstallBl2ProgramLocalsFastpath(DynarmicHost& host, bool startupEnabled = false);
void SetBl2ProgramLocalsEnabled(bool enabled);

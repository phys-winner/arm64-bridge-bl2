#pragma once
#include <cstdint>
#include <string>

class ThunkManager;
class DynarmicHost;

void RegisterOpenSLESThunks(ThunkManager& manager, DynarmicHost& host);
uint32_t GetOpenSLESInterfacePointer(const std::string& name);

// The Android real-time callback only records completions. Guest callbacks are
// dispatched from this safe host-side pump (or its dedicated non-RT worker).
void PumpOpenSLESCallbacks(DynarmicHost& host);
bool HasPendingAudioCallbacks();

void StartAudioThread(DynarmicHost* host);
void StopAudioThread();
void PauseAudioPlayback();
void ResumeAudioPlayback();

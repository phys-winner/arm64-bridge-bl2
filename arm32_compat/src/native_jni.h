#pragma once
#include <jni.h>
#include <cmath>
#include <cstdint>

// After dlopen, before the engine's original JNI_OnLoad creates any workers.
bool InstallNativeJniRuntimePatches();
// Published only after the original disconnected local save/achievement owners exist.
bool NativeLocalIdentityPrepared();
// Zero selects the genuine current Android Display rate, including fractional
// modes. An explicit user cap survives later Display mode callbacks.
inline float NativeFrameRateFor(float display, std::uint32_t requested) {
    if (!std::isfinite(display) || display<=0 || display>1000) return 0;
    return requested==0 ? display : (requested==30 || requested==60 || requested==90 ? float(requested) : 0);
}
float NativeCurrentFrameRateLimit();
bool NativeApplyFrameRateLimit();
// total=0 removes the first-launch preparation view, including failure/cancel.
void NativeShaderPreparationProgress(std::uint32_t completed, std::uint32_t total);
// After original JNI_OnLoad: replace only the callbacks owned by this adapter.
bool RegisterNativeActivityHooks(JNIEnv* env);

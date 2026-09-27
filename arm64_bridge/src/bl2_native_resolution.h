#pragma once

#include <cstddef>
#include <cstdint>
#include <jni.h>

class DynarmicHost;

// Verify the APK's mapped input before ELF loading, then patch only loaded
// guest memory before any renderer entry can execute. No persisted ELF edits.
bool VerifyBl2NativeResolutionInput(const uint8_t* data, size_t size);
bool InstallBl2NativeResolutionPatch(DynarmicHost& host);

// SurfaceHolder dimensions are the coordinate space used by the Java UI.
// Recover an invalid pair from this Surface's native window; otherwise reject
// the callback and leave the last valid renderer state intact.
bool PrepareBl2NativeResolutionSurface(JNIEnv* env, jobject surface,
                                       jint& width, jint& height);
void LogBl2NativeResolutionState(DynarmicHost& host, const char* event);

// Call after a successful eglMakeCurrent with a non-null current context,
// before returning to guest code. A failing real GL limit stops that guest
// thread before it can allocate unsupported scene targets.
void CheckBl2NativeResolutionLimits(DynarmicHost& host);

// Bounded provenance (first and 60th presentation after each surface callback).
// Call before eglSwapBuffers, while the final viewport is still current.
// This proves presentation dimensions, not the dimensions of an earlier FBO.
void AuditBl2NativeResolutionPresent(DynarmicHost& host);

#include "bl2_touch_input.h"
#include <jni.h>

using bl2::touch::sharedMailbox;
using bl2::touch::monotonicMilliseconds;

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeSnapshot(JNIEnv* env, jclass) {
    const auto wire = sharedMailbox().snapshot().wire();
    jlong values[8];
    for (std::size_t i = 0; i < wire.size(); ++i) values[i] = static_cast<jlong>(wire[i]);
    jlongArray result = env->NewLongArray(8);
    if (result != nullptr) env->SetLongArrayRegion(result, 0, 8, values);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeSubmit(
        JNIEnv*, jclass, jlong epoch, jfloat moveX, jfloat moveY,
        jfloat yawDegrees, jfloat pitchDegrees, jlong heldActions) {
    sharedMailbox().submit(static_cast<std::uint64_t>(epoch), moveX, moveY,
        yawDegrees, pitchDegrees, static_cast<std::uint64_t>(heldActions), monotonicMilliseconds());
}

extern "C" JNIEXPORT void JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeReset(JNIEnv*, jclass) {
    sharedMailbox().reset();
}

extern "C" JNIEXPORT void JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeUiPointer(
        JNIEnv*, jclass, jlong epoch, jint pointerId, jint phase,
        jfloat surfaceNormalizedX, jfloat surfaceNormalizedY) {
    if (phase < 0 || phase > 3) {
        // Preserve epoch isolation: a malformed stale packet must not reset a
        // newer scene. Mailbox validates this out-of-range enum under its lock.
        phase = 255;
    }
    sharedMailbox().uiPointer(static_cast<std::uint64_t>(epoch), pointerId,
        static_cast<bl2::touch::PointerPhase>(phase), surfaceNormalizedX,
        surfaceNormalizedY, monotonicMilliseconds());
}

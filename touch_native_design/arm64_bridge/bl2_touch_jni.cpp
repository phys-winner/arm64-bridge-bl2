#include "bl2_touch_input.h"
#include "../../arm64_bridge/src/bl2_touch_settings.h"
#include <jni.h>

using bl2::touch::sharedMailbox;
using bl2::touch::monotonicMilliseconds;

extern "C" JNIEXPORT jintArray JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeSettingsExchange(
        JNIEnv* env, jclass, jintArray packet) {
    constexpr jsize count = bl2::touch::kTouchSettingCount + 2;
    if (!packet || env->GetArrayLength(packet) != count) return nullptr;
    std::vector<std::uint32_t> host(count);
    env->GetIntArrayRegion(packet, 0, count, reinterpret_cast<jint*>(host.data()));
    if (env->ExceptionCheck()) return nullptr;
    const auto response = bl2::touch::ExchangeBl2TouchSettings(host);
    jintArray result = env->NewIntArray(static_cast<jsize>(response.size()));
    if (result) env->SetIntArrayRegion(result, 0, static_cast<jsize>(response.size()),
                                     reinterpret_cast<const jint*>(response.data()));
    return result;
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeSnapshot(JNIEnv* env, jclass) {
    const auto wire = sharedMailbox().snapshot().wire();
    std::array<jlong, bl2::touch::kSnapshotWords> values{};
    for (std::size_t i = 0; i < wire.size(); ++i) values[i] = static_cast<jlong>(wire[i]);
    const auto count = static_cast<jsize>(values.size());
    jlongArray result = env->NewLongArray(count);
    if (result != nullptr) env->SetLongArrayRegion(result, 0, count, values.data());
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeSubmit(
        JNIEnv*, jclass, jlong epoch, jfloat moveX, jfloat moveY,
        jfloat yawDegrees, jfloat pitchDegrees, jlong heldActions) {
    sharedMailbox().submit(static_cast<std::uint64_t>(epoch), moveX, moveY,
        yawDegrees, pitchDegrees, static_cast<std::uint64_t>(heldActions), monotonicMilliseconds());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeMovieState(JNIEnv*, jclass) {
    const auto access = bl2::touch::movieAccess();
    return access.state ? static_cast<jint>(access.state()) : 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeSkipMovie(JNIEnv*, jclass) {
    const auto access = bl2::touch::movieAccess();
    return access.skip && access.skip() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeShaderProgress(JNIEnv*, jclass) {
    return static_cast<jint>(bl2::touch::shaderFormationProgress());
}

extern "C" JNIEXPORT void JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeReset(JNIEnv*, jclass) {
    sharedMailbox().reset();
}

extern "C" JNIEXPORT void JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchBridge_nativeUiPointer(
        JNIEnv*, jclass, jlong epoch, jint pointerId, jint phase,
        jfloat surfaceNormalizedX, jfloat surfaceNormalizedY) {
    if (phase < 0 || phase > 4) {
        // Preserve epoch isolation: a malformed stale packet must not reset a
        // newer scene. Mailbox validates this out-of-range enum under its lock.
        phase = 255;
    }
    sharedMailbox().uiPointer(static_cast<std::uint64_t>(epoch), pointerId,
        static_cast<bl2::touch::PointerPhase>(phase), surfaceNormalizedX,
        surfaceNormalizedY, monotonicMilliseconds());
}

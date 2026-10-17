#pragma once

// Production builds are intentionally silent. Logging can still be enabled
// explicitly for a diagnostic bridge without touching individual call sites.
#ifndef HORN_ENABLE_LOGGING
#define HORN_ENABLE_LOGGING 0
#endif

#if HORN_ENABLE_LOGGING
#include <android/log.h>
#define HORN_LOGI(tag, ...) \
    __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define HORN_LOGE(tag, ...) \
    __android_log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
#else
#define HORN_LOGI(tag, ...) ((void)0)
#define HORN_LOGE(tag, ...) ((void)0)
#endif

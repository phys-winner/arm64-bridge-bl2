#include "bl2_silent_log.h"

#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <cstdarg>
#include <cstring>

// Compiled only into final builds. CMakeLists.txt links them with
// -Wl,--wrap=<function> for each liblog write function below, so every bridge
// call lands here and nothing is formatted or written.
extern "C" {
int __wrap___android_log_write(int, const char*, const char*) { return 0; }
int __wrap___android_log_print(int, const char*, const char*, ...) { return 0; }
int __wrap___android_log_vprint(int, const char*, const char*, va_list) { return 0; }
int __wrap___android_log_buf_write(int, int, const char*, const char*) { return 0; }
int __wrap___android_log_buf_print(int, int, const char*, const char*, ...) { return 0; }
}

namespace {

using SetLogger = void (*)(__android_logger_function);

std::atomic<__android_logger_function> g_logd_logger{nullptr};

bool OwnTag(const char* tag) {
    return tag && (std::strncmp(tag, "BL2", 3) == 0 || std::strncmp(tag, "Arm32Bridge", 11) == 0);
}

// Every liblog write of the process (android.util.Log included) ends here; other
// tags (the system's, AndroidRuntime crash reports) reach logd unchanged.
void FilterLogger(const struct __android_log_message* message) {
    if (OwnTag(message->tag)) return;
    if (const auto logger = g_logd_logger.load(std::memory_order_acquire)) logger(message);
}

}  // namespace

void Bl2SilenceProcessLogs() {
    // Both functions exist from API 30; the bridge targets API 24.
    const auto set_logger = reinterpret_cast<SetLogger>(dlsym(RTLD_DEFAULT, "__android_log_set_logger"));
    const auto logd_logger =
        reinterpret_cast<__android_logger_function>(dlsym(RTLD_DEFAULT, "__android_log_logd_logger"));
    if (!set_logger || !logd_logger) return;
    g_logd_logger.store(logd_logger, std::memory_order_release);
    set_logger(FilterLogger);
}

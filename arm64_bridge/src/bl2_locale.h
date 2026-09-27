#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

class DynarmicHost;

// Same range representation as the existing direct-APK libc reader. A retained
// descriptor is shared read-only; open() duplicates it and owns its own cursor.
struct Bl2LocaleEntry {
    char name[128];
    off_t data_offset;
    off_t file_size;
    int apk_fd;
};

// Cold-start only. Installed APK paths come from ApplicationInfo.splitSourceDirs;
// identity comes from the binary manifest, never from the /data/app filename.
// No extraction, voice-pack activation, Android locale marker, or hot swapping.
void IndexBl2LocaleSplits(const char* const* paths, size_t count,
                         const char* game_files_root);
bool VerifyBl2LocaleGuest(const uint8_t* data, size_t bytes);
bool InstallBl2LocaleOverlay(DynarmicHost& host);
const char* Bl2LocaleLanguage();  // Always "en"; verified ru_text replaces INT assets.

// Immutable entries after activation. Returned pointers live until process exit.
// The mutable type permits reuse of the existing ApkEntry reader without copying
// or TLS scratch storage; callers must not modify the returned records.
Bl2LocaleEntry* FindBl2LocaleEntry(const char* path);
bool IsBl2LocalePath(const char* path);
void NoteBl2LocaleOpen(const char* path, const char* operation);

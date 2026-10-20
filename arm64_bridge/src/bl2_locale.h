#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include "bl2_orchid_path_index.h"

struct stat;

class DynarmicHost;
class Bl2BinkAudio;

// Same range representation as the existing direct-APK libc reader. A retained
// descriptor is shared read-only; open() duplicates it and owns its own cursor.
struct Bl2LocaleEntry {
    char name[128];
    off_t data_offset;
    off_t file_size;
    int apk_fd;
    const Bl2BinkAudio* bink = nullptr;
};

// Cold-start only. Installed APK paths come from ApplicationInfo.splitSourceDirs;
// identity comes from the binary manifest, never from the /data/app filename.
// Text embedded in the base and the optional voice component activate for the
// Android app language on cold start, without extraction or locale markers.
void SetBl2LocaleLanguage(const char* language);
bool Bl2RussianLocaleSelected();
void IndexBl2LocaleSplits(const char* const* paths, size_t count,
                         const char* game_files_root);
bool IndexBl2CacheBase(const char* path);
void SetBl2LocaleMainObb(int fd, uint64_t offset, uint64_t bytes);
void SetBl2ExternalObbDirectory(const char* directory);
const char* Bl2ExternalObbDirectory();
bool HasBl2RelocatedCache();
// Engine identity is required independently of optional locale/DLC payloads.
bool VerifyBl2LocaleGuest(const uint8_t* data, size_t bytes);
bool InstallBl2LocaleOverlay(DynarmicHost& host);
const char* Bl2LocaleLanguage();  // Always "en"; Russian resources use the stock INT/audio namespace.

// Immutable entries after activation. Returned pointers live until process exit.
// The mutable type permits reuse of the existing ApkEntry reader without copying
// or TLS scratch storage; callers must not modify the returned records.
Bl2LocaleEntry* FindBl2LocaleEntry(const char* path);
bool IsBl2LocalePath(const char* path);
void NoteBl2LocaleOpen(const char* path, const char* operation);

// Internal cold-start publication only: caller has already verified/admitted
// the complete archive. No automatic archive selection, receipt or ownership
// decision is made here. The exact observed absolute DLC-directory path must
// include its final /DLC component; no Android root/cwd is inferred.
// Successful publication duplicates and retains source FDs for process life;
// returned entry/child pointers remain stable. A second publication is refused.
bool PublishBl2OrchidMount(const std::vector<bl2_orchid::FileRow>& verified_rows,
                          const char* observed_absolute_dlc_root);
bl2_orchid::NodeKind Bl2OrchidKind(const char* path);
const std::vector<bl2_orchid::Child>* Bl2OrchidChildren(const char* path);
// False means unhandled: the caller keeps its previous VFS behavior.
bool Bl2OrchidStat(const char* path, struct stat& output);
bool Bl2OrchidAccess(const char* path, int mode, int& result);

// Called immediately before the original libc directory scan. Reads the
// initialized original Android root, then publishes admitted immutable regions.
// No guessed root, guest call, license mutation or installation result override.
void EnsureBl2OrchidMount(DynarmicHost& host);
bool Bl2OrchidOwnsApp(uint32_t app_id);
// Read-only admission state, independent of deferred directory publication.
bool Bl2IrisContentAdmitted();
// Install last, after existing subtitle/locale hooks, to preserve old SVC IDs.
bool InstallBl2OrchidOwnership(DynarmicHost& host);

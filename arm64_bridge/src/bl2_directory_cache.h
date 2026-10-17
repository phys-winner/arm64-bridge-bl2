#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// A native directory listing translated by the existing ARM32 dirent writer.
// The cache never performs guest filename case matching itself.
struct Bl2DirectoryEntry {
    uint64_t inode = 0;
    int64_t offset = 0;
    uint8_t type = 0;
    char name[256]{};
};

struct Bl2DirectorySnapshot {
    std::vector<Bl2DirectoryEntry> entries;
};

// Production caches allowed asset directories; 0/unset/1 use that default.
// debug.bl2.dir_cache=2 observes the uncached native baseline instead.
// A hit is checked against a fresh stat(path).
// Literal "." is resolved against a fresh cwd, then the same allowlist applies.
// Null means use the existing native opendir/readdir/closedir implementation.
// Embedded OBB directory virtualization must bypass this helper.
std::shared_ptr<const Bl2DirectorySnapshot> Bl2GetDirectorySnapshot(const char* path);
void Bl2DirectoryCacheRecordRead(bool snapshot);
void Bl2DirectoryCacheRecordClose();
// Call at the first real swap to retain exact startup counters for either mode.
void Bl2DirectoryCacheReportStartup();

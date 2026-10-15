#pragma once

#include <cstdint>
#include <sys/types.h>

// Which cooked package a guest read of an original OBB lands in. While shader
// formation runs, a level package's first read tells it which level is
// loading: its stages are formed during the loading screen, before the first
// frame creates them. Without active formation every call returns at once.
//
// Guest open of main/patch OBB `obb` (0 main, 1 patch): `guest_fd` is what the
// guest reads; the index is read from `read_fd` at `base_offset` (an APK entry
// or the external file itself).
void Bl2ObbPackageWatchOpen(int guest_fd, int obb, int read_fd, off_t base_offset);
// Before a guest read at `offset` within the OBB opened as `guest_fd`.
void Bl2ObbPackageWatchRead(int guest_fd, uint64_t offset);
void Bl2ObbPackageWatchClose(int guest_fd);
// The guest fd is a watched OBB (native reads need its offset from lseek).
bool Bl2ObbPackageWatched(int guest_fd);

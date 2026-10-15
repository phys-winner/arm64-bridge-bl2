#pragma once

#include <cstddef>
#include <cstdint>

#define HORN_LOGI(...) ((void)0)
#define HORN_LOGE(...) ((void)0)

#if defined(_WIN32)
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
using ssize_t = int;
inline int Bl2LocaleHostPread(int fd, void* data, size_t size, int64_t offset) {
    return _lseeki64(fd, offset, SEEK_SET) == offset
        ? _read(fd, data, static_cast<unsigned>(size)) : -1;
}
#define pread Bl2LocaleHostPread
#define close _close
#define dup _dup
#define open _open
#define fstat _fstat64
#define stat _stat64
#define off_t int64_t
#define O_CLOEXEC 0
#undef O_RDONLY
#define O_RDONLY (_O_RDONLY | _O_BINARY)
#else
#include <unistd.h>
#endif

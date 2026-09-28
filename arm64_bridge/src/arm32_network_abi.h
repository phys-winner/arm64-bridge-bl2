#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

// Legacy Bionic layouts. Pointer fields contain guest addresses, never host pointers.
struct Arm32AddrInfo {
    int32_t flags, family, socktype, protocol;
    uint32_t addrlen, canonname, addr, next;
};
struct Arm32HostEnt {
    uint32_t name, aliases;
    int32_t addrtype, length;
    uint32_t addr_list;
};
struct Arm32IfConf { int32_t length; uint32_t buffer; };
struct Arm32Timeval { int32_t seconds, microseconds; };
struct Arm32FdSet { uint32_t words[32]; };
constexpr size_t kArm32IfReqSize = 32;

static_assert(sizeof(Arm32AddrInfo) == 32);
static_assert(offsetof(Arm32AddrInfo, canonname) == 20);
static_assert(offsetof(Arm32AddrInfo, addr) == 24);
static_assert(offsetof(Arm32AddrInfo, next) == 28);
static_assert(sizeof(Arm32HostEnt) == 20);
static_assert(sizeof(Arm32IfConf) == 8);
static_assert(sizeof(Arm32Timeval) == 8);
static_assert(sizeof(Arm32FdSet) == 128);

template<class NativeTimeval>
bool UnpackArm32Timeval(const Arm32Timeval& guest, NativeTimeval& native) {
    if (guest.seconds < 0 || guest.microseconds < 0 || guest.microseconds >= 1000000)
        return false;
    native.tv_sec = guest.seconds;
    native.tv_usec = guest.microseconds;
    return true;
}

template<class NativeTimeval>
bool PackArm32Timeval(const NativeTimeval& native, Arm32Timeval& guest) {
    if (native.tv_sec < 0 || native.tv_sec > std::numeric_limits<int32_t>::max() ||
        native.tv_usec < 0 || native.tv_usec >= 1000000) return false;
    guest = {static_cast<int32_t>(native.tv_sec), static_cast<int32_t>(native.tv_usec)};
    return true;
}

template<class Word, size_t N>
void UnpackArm32FdSet(const Arm32FdSet& guest, Word (&native)[N]) {
    static_assert(sizeof(native) == sizeof(guest));
    static_assert(sizeof(Word) == 4 || sizeof(Word) == 8);
    constexpr size_t parts = sizeof(Word) / 4;
    for (size_t i = 0; i < N; ++i) {
        native[i] = 0;
        for (size_t j = 0; j < parts; ++j)
            native[i] |= static_cast<Word>(guest.words[i * parts + j]) << (32 * j);
    }
}

template<class Word, size_t N>
void PackArm32FdSet(const Word (&native)[N], Arm32FdSet& guest) {
    static_assert(sizeof(native) == sizeof(guest));
    constexpr size_t parts = sizeof(Word) / 4;
    for (size_t i = 0; i < N; ++i)
        for (size_t j = 0; j < parts; ++j)
            guest.words[i * parts + j] = static_cast<uint32_t>(native[i] >> (32 * j));
}

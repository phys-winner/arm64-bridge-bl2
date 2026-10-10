#include "arm32_network_abi.h"

#include <cassert>
#include <cstring>
#include <initializer_list>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    // Canary proves the 16-byte host timeval never overwrites the 8-byte guest one.
    struct NativeTimeval { int64_t tv_sec, tv_usec; } native{};
    struct { Arm32Timeval value; uint32_t canary; } guest{{2, 125000}, 0x12345678};
    assert(UnpackArm32Timeval(guest.value, native));
    assert(native.tv_sec == 2 && native.tv_usec == 125000);
    native = {1, 999999};
    assert(PackArm32Timeval(native, guest.value));
    assert(guest.value.seconds == 1 && guest.value.microseconds == 999999);
    assert(guest.canary == 0x12345678);
    assert(!UnpackArm32Timeval(Arm32Timeval{-1, 0}, native));
    assert(!UnpackArm32Timeval(Arm32Timeval{0, 1000000}, native));
    native = {int64_t(INT32_MAX) + 1, 0};
    assert(!PackArm32Timeval(native, guest.value));

    Arm32FdSet input{}, output{};
    for (unsigned fd : {0u, 31u, 32u, 63u, 64u, 1023u})
        input.words[fd / 32] |= uint32_t(1) << (fd % 32);
    uint64_t wide[16]{};
    UnpackArm32FdSet(input, wide);
    assert(wide[0] == 0x8000000180000001ULL);
    assert(wide[1] == 1 && wide[15] == (uint64_t(1) << 63));
    wide[0] &= ~(uint64_t(1) << 32);
    PackArm32FdSet(wide, output);
    assert(output.words[0] == 0x80000001 && output.words[1] == 0x80000000);
    uint32_t narrow[32]{};
    UnpackArm32FdSet(input, narrow);
    PackArm32FdSet(narrow, output);
    assert(std::memcmp(&input, &output, sizeof(input)) == 0);

    // Bionic ai_canonname precedes ai_addr; glibc's order is different.
    uint32_t words[] = {1, 2, 2, 17, 16, 0x1000, 0x2000, 0x3000};
    Arm32AddrInfo info{};
    std::memcpy(&info, words, sizeof(info));
    assert(info.canonname == 0x1000 && info.addr == 0x2000 && info.next == 0x3000);
    assert(kArm32IfReqSize == 32);

    // The converted set must let the real kernel report a pending UDP datagram.
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0 && fd < 1024);
    int enabled = 1;
    assert(setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0);
    assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t address_length = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_length) == 0);
    constexpr char payload[] = "arm32-network-abi";
    assert(sendto(fd, payload, sizeof(payload), 0,
                  reinterpret_cast<sockaddr*>(&address), sizeof(address)) == sizeof(payload));
    Arm32FdSet requested{}, ready_set{};
    requested.words[fd / 32] |= uint32_t(1) << (fd % 32);
    fd_set host_set{};
    UnpackArm32FdSet(requested, host_set.fds_bits);
    timeval host_timeout{};
    assert(UnpackArm32Timeval(Arm32Timeval{1, 0}, host_timeout));
    assert(select(fd + 1, &host_set, nullptr, nullptr, &host_timeout) == 1);
    PackArm32FdSet(host_set.fds_bits, ready_set);
    assert(ready_set.words[fd / 32] & (uint32_t(1) << (fd % 32)));
    char received[sizeof(payload)]{};
    assert(recvfrom(fd, received, sizeof(received), 0, nullptr, nullptr) == sizeof(payload));
    assert(std::memcmp(received, payload, sizeof(payload)) == 0);
    assert(close(fd) == 0);
}

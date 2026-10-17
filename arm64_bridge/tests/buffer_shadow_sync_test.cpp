#include "buffer_shadow_sync.h"
#include <array>
#include <cstdio>

int main() {
    constexpr size_t page = 4096;
    constexpr size_t size = 3 * page + 17;
    constexpr uint8_t guard = 0xA5;
    std::array<uint8_t, size + 2> native{}, shadow{}, guest{};
    native.fill(guard);
    shadow.front() = shadow.back() = guard;
    guest.front() = guest.back() = guard;
    unsigned cases = 0, failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        ++cases;
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL %s\n", name);
        }
    };
    const auto sync = [&](bool force = false) {
        return SyncChangedBufferChunks(native.data() + 1, shadow.data() + 1,
                                       guest.data() + 1, size, force);
    };
    expect(sync(true) == size, "first zero-filled upload is forced");
    expect(native == guest && shadow == guest, "forced upload equals guest");
    expect(sync() == 0, "unchanged data is skipped");
    guest[1] = 0x12;
    expect(sync() == page, "dirty first page");
    expect(native == guest && shadow == guest, "first-page upload equality");
    guest[page] = 0x34;
    guest[page + 1] = 0x56;
    expect(sync() == 2 * page, "write straddling page boundary");
    expect(native == guest && shadow == guest, "cross-page equality");
    guest[size] = 0x78;
    expect(sync() == 17, "short final page");
    expect(native == guest && shadow == guest, "tail equality and sentinels");
    guest[2 * page + 42] = 0x9A;
    expect(sync() == page, "isolated middle page");
    expect(sync() == 0, "updated shadow suppresses second copy");
    guest[2 * page + 42] = 0;
    expect(sync() == page, "writes returning to zero are not skipped");
    expect(native == guest && shadow == guest, "all final data and guards");
    expect(SyncChangedBufferChunks(nullptr, nullptr, nullptr, 0, true) == 0,
           "empty range");
    std::printf("buffer_shadow_sync: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}

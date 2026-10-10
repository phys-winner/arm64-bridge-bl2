#include "bl2_guest_integrity.h"

#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"

#include <android/log.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/system_properties.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr const char* kTag = "BL2Integrity";
// Pinned guest (image 0x03089000): PT_GNU_RELRO covers .data.rel.ro.local,
// .init_array, .data.rel.ro, .fini_array, .dynamic and .got.
constexpr uint32_t kImageSize = 0x03089000;
constexpr uint32_t kRelroRva = 0x02D209D0;
constexpr uint32_t kRelroBytes = 0x219630;
constexpr auto kPeriod = std::chrono::milliseconds(250);
constexpr uint32_t kSvc = 0xEF000000u;
constexpr uint32_t kBxLr = 0xE12FFF1Eu;
constexpr uint32_t kMaxReports = 64;

void Watch(DynarmicHost* host, uint32_t relro, std::vector<uint8_t> snapshot) {
    pthread_setname_np(pthread_self(), "BL2Integrity");
    setpriority(PRIO_PROCESS, 0, 10);
    ThunkManager& thunks = host->GetThunkManager();
    uint32_t reports = 0;
    const auto report = [&](const char* format, auto... args) {
        if (reports < kMaxReports) {
            ++reports;
            __android_log_print(ANDROID_LOG_ERROR, kTag, format, args...);
        }
    };
    for (;;) {
        std::this_thread::sleep_for(kPeriod);
        const uint8_t* live = host->GetGuestMemoryBase() + relro;
        if (std::memcmp(live, snapshot.data(), snapshot.size()) != 0) {
            for (size_t offset = 0; offset + 4 <= snapshot.size(); offset += 4) {
                uint32_t now = 0, was = 0;
                std::memcpy(&now, live + offset, 4);
                std::memcpy(&was, snapshot.data() + offset, 4);
                if (now == was) continue;
                report("RELRO word 0x%08X changed %08X -> %08X; restored",
                       relro + static_cast<uint32_t>(offset), was, now);
                host->MemoryWrite32(relro + static_cast<uint32_t>(offset), was);
            }
        }
        const uint32_t base = ThunkManager::TrampolineBase();
        const uint32_t stride = ThunkManager::TrampolineStride();
        const uint32_t end = thunks.TrampolineEnd();
        for (uint32_t address = base; address + stride <= end; address += stride) {
            const uint32_t svc = kSvc | (1 + (address - base) / stride);
            const uint32_t first = host->MemoryRead32(address);
            const uint32_t second = host->MemoryRead32(address + 4);
            if (first == svc && second == kBxLr) continue;
            report("trampoline 0x%08X was %08X %08X (want %08X %08X); restored",
                   address, first, second, svc, kBxLr);
            host->WriteLowGuardWord(address, svc);
            host->WriteLowGuardWord(address + 4, kBxLr);
            host->InvalidateGuestCode(address, stride);
        }
    }
}

}  // namespace

void StartBl2GuestIntegrityWatch(DynarmicHost& host) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2.integrity", value) > 0 && value[0] == '0') return;
    ElfLoader& loader = host.GetElfLoader();
    const uint64_t relro = uint64_t(loader.GetBaseAddress()) + kRelroRva;
    if (loader.GetImageSize() != kImageSize || relro + kRelroBytes > host.GetGuestMemorySize()) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "unexpected guest layout; watch disabled");
        return;
    }
    const uint8_t* memory = host.GetGuestMemoryBase();
    std::vector<uint8_t> snapshot(memory + relro, memory + relro + kRelroBytes);
    try {
        std::thread(Watch, &host, static_cast<uint32_t>(relro), std::move(snapshot)).detach();
    } catch (...) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "watching RELRO 0x%08X+0x%X and trampolines",
                        static_cast<uint32_t>(relro), kRelroBytes);
}

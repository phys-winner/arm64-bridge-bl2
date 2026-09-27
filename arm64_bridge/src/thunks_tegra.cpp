#include "thunks_tegra.h"

#include "bridge_log.h"
#include "dynarmic_host.h"
#include "thunk_manager.h"
#include <dynarmic/interface/A32/a32.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#define LOG_TAG "Arm32Bridge_Tegra"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

namespace {

struct UnavailableTegraApi {
    const char* name;
    uint32_t result;
};

constexpr uint32_t kDeviceUnavailable =
    static_cast<uint32_t>(-ENODEV);

constexpr UnavailableTegraApi kUnavailableApis[] = {
    {"TSecOpenChannel", kDeviceUnavailable},
    {"TSecCloseChannel", kDeviceUnavailable},
    {"TSecMemAlloc", 0},
    {"TSecMemWrite", kDeviceUnavailable},
    {"TSecInitStream", kDeviceUnavailable},
    {"TSecMemFree", 0},
    {"TSecCloseStream", kDeviceUnavailable},
    {"TSecStreamWait", kDeviceUnavailable},
    {"TSecStartStreaming", kDeviceUnavailable},
    {"TSecWriteToStreamReloc", kDeviceUnavailable},
    {"TSecWriteToStream", kDeviceUnavailable},
    {"TSecStopStreamingAndFlush", kDeviceUnavailable},
};

}  // namespace

bool InstallBorderlandsNvsiLifecycle(DynarmicHost& host) {
    // NVSI's Java wrapper requires PENDING (1) from InitEx, then polls for OK
    // (0). Returning zero directly from InitEx takes the fatal-error branch.
    // Values/signatures were checked against this title's original ELF in IDA.
    // Lock/Unlock retain the original guest mutex; no hardware or server worker
    // is started by these replacement lifecycle calls.
    enum : uint32_t { kOk = 0, kPending = 1, kInvalid = 2, kPaused = 4,
                      kBadParameter = 8 };
    enum class Operation { Init, Check, Pause, Resume, Close };
    struct Hook {
        const char* symbol;
        std::array<uint32_t, 2> signature;
        Operation operation;
    };
    constexpr std::array<Hook, 5> hooks = {{
        {"nvsiInitEx", {0xE92D43F0, 0xE1A07000}, Operation::Init},
        {"nvsiCheckStatus", {0xE92D4030, 0xE24DD00C}, Operation::Check},
        {"nvsiPause", {0xE92D4010, 0xE59F4058}, Operation::Pause},
        {"nvsiResume", {0xE92D4070, 0xE24DD008}, Operation::Resume},
        {"nvsiClose", {0xE92D4070, 0xE24DD018}, Operation::Close},
    }};
    std::array<uint32_t, hooks.size()> functions{};
    std::array<uint32_t, hooks.size()> trampolines{};
    for (size_t i = 0; i < hooks.size(); ++i) {
        const uint32_t address = host.FindSymbol(hooks[i].symbol);
        if (!address || (address & 3) ||
            static_cast<uint64_t>(address) + 8 > host.GetGuestMemorySize() ||
            host.MemoryRead32(address) != hooks[i].signature[0] ||
            host.MemoryRead32(address + 4) != hooks[i].signature[1]) {
            LOGE("NVSI lifecycle patch refused: %s signature mismatch at 0x%08X",
                 hooks[i].symbol, address);
            return false;
        }
        functions[i] = address;
    }
    auto status = std::make_shared<std::atomic<uint32_t>>(kPending);
    for (size_t i = 0; i < hooks.size(); ++i) {
        const Hook hook = hooks[i];
        trampolines[i] = host.GetThunkManager().RegisterThunk(
            std::string("bl2_local_") + hook.symbol,
            [status, hook](Dynarmic::A32::Jit& jit, DynarmicHost&) {
                uint32_t result = status->load(std::memory_order_acquire);
                switch (hook.operation) {
                    case Operation::Init:
                        if (jit.Regs()[0] == 0) {
                            result = kBadParameter;
                        } else {
                            status->store(kOk, std::memory_order_release);
                            result = kPending;
                            LOGI("NVSI local lifecycle initialized: shared_kib=%u flags=%u; "
                                 "TSEC snippets remain unsupported",
                                 jit.Regs()[0], jit.Regs()[1]);
                        }
                        break;
                    case Operation::Check:
                        break;
                    case Operation::Pause:
                        if (result == kOk) {
                            result = kPaused;
                            status->store(result, std::memory_order_release);
                        }
                        break;
                    case Operation::Resume:
                        if (result == kPaused) {
                            result = kOk;
                            status->store(result, std::memory_order_release);
                        }
                        break;
                    case Operation::Close:
                        status->store(kInvalid, std::memory_order_release);
                        result = kOk;
                        break;
                }
                jit.Regs()[0] = result;
            });
        if (!trampolines[i]) return false;
    }
    // All signatures and registrations were validated before changing any code.
    constexpr uint32_t kArmLdrPcLiteral = 0xE51FF004;
    for (size_t i = 0; i < hooks.size(); ++i) {
        host.MemoryWrite32(functions[i], kArmLdrPcLiteral);
        host.MemoryWrite32(functions[i] + 4, trampolines[i]);
        host.InvalidateGuestCode(functions[i], 8);
        if (host.MemoryRead32(functions[i]) != kArmLdrPcLiteral ||
            host.MemoryRead32(functions[i] + 4) != trampolines[i]) return false;
        LOGI("NVSI local lifecycle hook %s at 0x%08X -> 0x%08X",
             hooks[i].symbol, functions[i], trampolines[i]);
    }
    return true;
}

void RegisterUnavailableTegraThunks(ThunkManager& manager) {
    // The supplied libtsec_wrapper.so implements this export as a guest-owned
    // static string containing "t114" (confirmed from its ARM32 body). Return
    // the same value from guest memory; a host pointer would be truncated.
    auto version_address = std::make_shared<uint32_t>(0);
    auto version_mutex = std::make_shared<std::mutex>();
    manager.RegisterThunk("TSecGetTegraVersion",
        [version_address, version_mutex](Dynarmic::A32::Jit& jit,
                                        DynarmicHost& host) {
            std::lock_guard<std::mutex> lock(*version_mutex);
            if (*version_address == 0) {
                static constexpr char kTegraVersion[] = "t114";
                const uint32_t address =
                    host.GuestAlloc(sizeof(kTegraVersion), alignof(uint32_t));
                if (address == 0) {
                    LOGE("Could not allocate guest storage for TSec version string");
                    jit.Regs()[0] = 0;
                    return;
                }
                std::memcpy(host.GetGuestMemoryBase() + address,
                            kTegraVersion, sizeof(kTegraVersion));
                *version_address = address;
                LOGI("TSecGetTegraVersion -> '%s' at guest 0x%08X",
                     kTegraVersion, address);
            }
            jit.Regs()[0] = *version_address;
        });

    for (const auto& api : kUnavailableApis) {
        manager.RegisterThunk(api.name, [api](Dynarmic::A32::Jit& jit,
                                               DynarmicHost&) {
            static std::atomic<uint32_t> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
                LOGE("Tegra-only import %s called without nvhost-tsec; returning 0x%08X",
                     api.name, api.result);
            }
            jit.Regs()[0] = api.result;
        });
    }
}

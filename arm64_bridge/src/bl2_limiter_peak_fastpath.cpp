#include "bl2_limiter_peak_fastpath.h"

#include "bl2_limiter_peak_scan.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "obb_sha256.h"
#include "thunk_manager.h"

#include <dynarmic/interface/A32/a32.h>
#include <sys/system_properties.h>

#include <cstdint>
#include <cstring>
#include <atomic>

namespace {
constexpr char kSymbol[] = "_Z27McDSPLimiterProcessChannelsPvPKfS1_PfS2_m";
constexpr char kBodySha[] =
    "EBBED5126689A76411B3B9888438E5885F3256889E903E2570A25CA1CA4C02F9";
constexpr uint32_t kFunctionRva = 0x0270E910;
constexpr uint32_t kFunctionSize = 0x6A0;
constexpr uint32_t kLoopOffset = 0x2AC;
constexpr uint32_t kLoopSize = 0x28;
constexpr uint32_t kOriginalVabs = 0xEEF07AC6;
constexpr uint32_t kArmLdrPcLiteral = 0xE51FF004;
std::atomic<bool> enabled{false};

uint32_t ReverseBytes(uint32_t value) {
    return __builtin_bswap32(value);
}
}

void SetBl2LimiterPeakEnabled(bool value) {
    enabled.store(value,std::memory_order_relaxed);
}
bool InstallBl2LimiterPeakFastpath(DynarmicHost& host, bool startupEnabled) {
    char property[PROP_VALUE_MAX] = {};
    const auto length=__system_property_get("debug.bl2.limiter_peak",property);
    if(length)startupEnabled=length==1 && property[0]=='1';
    if(!startupEnabled)return true;

    // JNI calls this after verifying the entire original ELF and before any
    // guest execution. Also certify the complete function, not just the SVC
    // site: its register and DSP-bank contract comes from this exact body.
    auto& loader = host.GetElfLoader();
    const uint32_t function = host.FindSymbol(kSymbol);
    const uint64_t expected = uint64_t(loader.GetBaseAddress()) + kFunctionRva;
    if (host.IsJitRunning() || loader.GetImageSize() != 0x03089000 ||
        expected != function || !function ||
        uint64_t(function) + kFunctionSize > host.GetGuestMemorySize() ||
        !loader.IsReadOnlyRange(function, kFunctionSize)) return false;
    ObbSha256 digest;
    digest.Update(host.GetGuestMemoryBase() + function, kFunctionSize);
    const auto actual = ObbSha256::Hex(digest.Finalize());
    if (std::strcmp(actual.data(), kBodySha) != 0) return false;

    const uint32_t site = function + kLoopOffset;
    const uint32_t fallback = host.GuestAlloc(24, 4);
    if (!fallback) return false;
    // GuestAlloc can be much farther than ARM B's +/-32 MiB reach. Both
    // veneers use LDR PC without touching GPRs/LR/flags. E=1 byte-reverses
    // the data literal, so it uses a separate literal with reversed bytes.
    for (uint32_t offset : {0u, 12u}) {
        host.MemoryWrite32(fallback + offset, kOriginalVabs);
        host.MemoryWrite32(fallback + offset + 4, kArmLdrPcLiteral);
        host.MemoryWrite32(fallback + offset + 8,
            offset ? ReverseBytes(site + 4) : site + 4);
    }
    host.InvalidateGuestCode(fallback, 24);

    const uint32_t thunk = host.GetThunkManager().RegisterThunk(
        Bl2LimiterPeak::kThunkName,
        [site, fallback](Dynarmic::A32::Jit& jit, DynarmicHost& thunk_host) {
            // A loop-site SVC supplies site+4 and leaves the original LR.
            // This handler is not a normal function thunk ending in BX LR.
            if (jit.Regs()[15] != site + 4) {
                jit.HaltExecution(Dynarmic::HaltReason::UserDefined3);
                return;
            }
            uint32_t cpsr = jit.Cpsr(), fpscr = jit.Fpscr();
            const uint64_t bank = jit.Regs()[0];
            // The malloc arena is anonymous RW for its complete lifetime;
            // file-backed mmap lives in a separate arena starting at 0x80000000.
            // Do not perform direct host reads from an uncertified bank.
            const bool anonymous_bank =
                DynarmicHost::IsHeapRange(bank, Bl2LimiterPeak::kStateSize);
            if (enabled.load(std::memory_order_relaxed) && anonymous_bank && Bl2LimiterPeak::Try(
                    thunk_host.GetGuestMemoryBase(), thunk_host.GetGuestMemorySize(),
                    jit.Regs(), jit.ExtRegs(), cpsr, fpscr, site + kLoopSize)) {
                jit.SetCpsr(cpsr);
                jit.SetFpscr(fpscr);
            } else {
                // Try rejects before any register/memory update. Execute the
                // displaced VABS and return to the unmodified loop body.
                jit.Regs()[15] = fallback + ((cpsr & 0x200u) ? 12u : 0u);
            }
        });
    if (!thunk) {
        host.GuestFree(fallback);
        return false;
    }
    const uint32_t svc = host.MemoryRead32(thunk);
    if ((svc & 0xFF000000u) != 0xEF000000u || !(svc & 0x00FFFFFFu)) {
        // The registered closure still owns its veneer. Keep the original
        // loop and do not free memory reachable by that closure.
        return false;
    }
    host.AddExecutableRange(fallback, 24);
    host.MemoryWrite32(site, svc);
    host.InvalidateGuestCode(site, 4);
    enabled.store(true,std::memory_order_relaxed);
    HORN_LOGI("Arm32Bridge_LimiterPeak", "Limiter peak SVC installed at 0x%08X", site);
    return true;
}

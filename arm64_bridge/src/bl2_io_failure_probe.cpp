#include "bl2_io_failure_probe.h"

#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "obb_sha256.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/system_properties.h>
#include <unistd.h>

namespace {
constexpr char kTag[] = "BL2_IOFailure";
constexpr char kSha[] = "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814";
constexpr std::size_t kFileBytes = 49885152;
constexpr std::uint32_t kImageBytes = 0x03089000;
constexpr std::uint32_t kFailureRva = 0x01E1580C;
constexpr std::uint32_t kLogReturnRva = 0x01E1583C;
constexpr std::array<std::uint32_t, 20> kFailureWords{
    0xE3500000, 0xE92D4008, 0x11A03000, 0x0A000008,
    0xE59FC028, 0xE3A00003, 0xE59F1024, 0xE59F2024,
    0xE79FC00C, 0xE08F1001, 0xE08F2002, 0xE12FFF3C,
    0xE8BD8008, 0xE59F3010, 0xE08F3003, 0xEAFFFFF3,
    0x01117260, 0x00E1E4A0, 0x00E1E4DC, 0x00E1E4BC
};
std::atomic<bool> g_raw_verified{false};
std::atomic<unsigned> g_failures{0};

struct ErrnoGuard {
    int saved = errno;
    ~ErrnoGuard() { errno = saved; }
};

// Corrupt frame pointers must not crash a diagnostic. /proc/self/mem is the
// same bounded safe-read seam used by the existing stack-guard diagnostic.
struct Reader {
    DynarmicHost& host;
    int fd = -1;
    explicit Reader(DynarmicHost& value) : host(value) {
        fd = ::open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    }
    ~Reader() { if (fd >= 0) ::close(fd); }
    bool bytes(std::uint64_t address, void* output, std::size_t size) const {
        const std::uint64_t arena = host.GetGuestMemorySize();
        const std::uint64_t base = reinterpret_cast<std::uintptr_t>(host.GetGuestMemoryBase());
        const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (fd < 0 || address == 0 || address > arena || size > arena - address ||
            base > limit || address > limit - base || size > limit - base - address) return false;
        return ::pread64(fd, output, size, static_cast<off64_t>(base + address)) ==
               static_cast<ssize_t>(size);
    }
    template<std::size_t N>
    bool words(std::uint64_t address, std::array<std::uint32_t, N>& output) const {
        return bytes(address, output.data(), sizeof(output));
    }
};

const char* FulfillReason(std::uint32_t caller_rva) {
    switch (caller_rva) {
    case 0x00F688E8: return "chunk-magic";
    case 0x00F68A80: return "chunk-uncompressed-sum";
    case 0x00F68AB8: return "compressed-extent";
    case 0x00F68AE4: return "request-uncompressed-size";
    default: return "other-caller";
    }
}
} // namespace

bool VerifyBl2IoFailureProbeInput(const std::uint8_t* bytes, std::size_t size) {
    const ErrnoGuard errno_guard;
    g_raw_verified.store(false, std::memory_order_release);
    g_failures.store(0, std::memory_order_relaxed);
    char enabled[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.bl2.io_failure_probe", enabled) <= 0 ||
        std::strcmp(enabled, "1") != 0 || !bytes || size != kFileBytes) return false;
    ObbSha256 sha;
    if (!sha.Update(bytes, size)) return false;
    const auto actual = ObbSha256::Hex(sha.Finalize());
    const bool valid = std::strcmp(actual.data(), kSha) == 0;
    g_raw_verified.store(valid, std::memory_order_release);
    HORN_LOGI(kTag, "IO_PROBE raw_sha=%s verified=%u max_failures=12 guest_hook=0",
              actual.data(), valid ? 1u : 0u);
    return valid;
}

void ObserveBl2IoFailureLog(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    if (!g_raw_verified.load(std::memory_order_acquire) ||
        g_failures.load(std::memory_order_relaxed) >= 12) return;
    const auto regs = jit.Regs();
    const auto& elf = host.GetElfLoader();
    const std::uint32_t base = elf.GetBaseAddress();
    if (std::uint64_t(base) + kImageBytes > host.GetGuestMemorySize() ||
        regs[14] != std::uint64_t(base) + kLogReturnRva) return;
    const unsigned sample = g_failures.fetch_add(1, std::memory_order_relaxed);
    if (sample >= 12) return;
    const ErrnoGuard errno_guard;
    Reader read(host);
    std::array<std::uint32_t, kFailureWords.size()> code{};
    const bool signature = elf.GetImageSize() == kImageBytes &&
        elf.GetSymbolAddress("_Z18appHandleIOFailurePKc") == base + kFailureRva &&
        read.words(std::uint64_t(base) + kFailureRva, code) && code == kFailureWords;
    if (!signature) {
        HORN_LOGI(kTag, "IO_PROBE failure=%u signature=0 base=%08X lr=%08X sp=%08X reader=%d",
                  sample + 1, base, regs[14], regs[13], read.fd >= 0 ? 1 : 0);
        g_raw_verified.store(false, std::memory_order_release);
        return;
    }

    // appHandleIOFailure pushes {r3,lr}; at its log call SP+4 is the actual
    // caller's LR, whereas current LR always points to appHandleIOFailure+0x30.
    std::array<std::uint32_t, 2> saved{};
    const bool saved_ok = read.words(regs[13], saved);
    const std::uint32_t caller = saved_ok ? saved[1] : 0;
    const std::uint32_t caller_rva = caller >= base ? caller - base : 0;
    const char* reason = FulfillReason(caller_rva);
    const bool fulfill = std::strcmp(reason, "other-caller") != 0;
    char filename[193]{};
    const bool filename_ok = read.bytes(regs[3], filename, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = 0;

    // These offsets are valid only for the four verified FulfillCompressedRead
    // calls above. Its SP is current SP+8. Avoid decoding a different frame.
    std::array<std::uint32_t, 51> frame{};
    std::array<std::uint32_t, 14> request{};
    std::array<std::uint32_t, 3> handle{}, oob{}, entry{};
    std::array<std::uint32_t, 1> entry_pointer{};
    std::array<std::uint32_t, 6> chunks{};
    const bool frame_ok = fulfill && read.words(std::uint64_t(regs[13]) + 8, frame);
    const bool request_ok = fulfill && read.words(regs[8], request);
    const bool handle_ok = frame_ok && read.words(frame[0x28 / 4], handle);
    const bool oob_ok = handle_ok && read.words(handle[0], oob);
    const bool entry_ok = oob_ok && read.words(oob[2], entry_pointer) &&
        read.words(entry_pointer[0], entry);
    const bool chunks_ok = fulfill && caller_rva != 0x00F688E8 && read.words(regs[5], chunks);
    HORN_LOGI(kTag,
        "IO_PROBE failure=%u signature=1 caller=%08X caller_rva=%08X reason=%s "
        "lr=%08X sp=%08X saved_ok=%u filename_ok=%u file='%s' "
        "r0_3={%08X,%08X,%08X,%08X} r4_11={%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X} "
        "frame_ok=%u header={%08X,%08X} chunk_count=%08X "
        "request_ok=%u request=%08X fileptr=%08X filelen=%08X offset=%08X compressed=%08X "
        "uncompressed=%08X dest=%08X compression_flags=%08X "
        "handle_ok=%u handle={%08X,%08X,%08X} oob_ok=%u oob={%08X,%08X,%08X} "
        "entry_ok=%u entry={%08X,%08X,%08X} chunks_ok=%u chunks={%08X,%08X,%08X,%08X,%08X,%08X}",
        sample + 1, caller, caller_rva, reason, regs[14], regs[13],
        saved_ok ? 1u : 0u, filename_ok ? 1u : 0u, filename_ok ? filename : "unreadable",
        regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7],
        regs[8], regs[9], regs[10], regs[11], frame_ok ? 1u : 0u,
        frame[0x60 / 4], frame[0x64 / 4], caller_rva == 0x00F688E8 ? 0u : frame[0x2C / 4],
        request_ok ? 1u : 0u, regs[8], request[2], request[3], request[5], request[6],
        request[7], request[8], request[9], handle_ok ? 1u : 0u, handle[0], handle[1], handle[2],
        oob_ok ? 1u : 0u, oob[0], oob[1], oob[2], entry_ok ? 1u : 0u,
        entry[0], entry[1], entry[2], chunks_ok ? 1u : 0u,
        chunks[0], chunks[1], chunks[2], chunks[3], chunks[4], chunks[5]);
}

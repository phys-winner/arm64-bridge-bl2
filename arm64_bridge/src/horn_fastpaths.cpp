#include "horn_fastpaths.h"

#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define LOG_TAG "Arm32Bridge_FastPaths"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

namespace {

constexpr char kGetPackedProgramKeySymbol[] =
    "_ZN15FProgramKeyData19GetPackedProgramKeyER11FProgramKey";
constexpr char kProgramKeyFields0Symbol[] =
    "_ZN15FProgramKeyData26ES2ShaderProgramKeyFields0E";
constexpr char kProgramKeyFields1Symbol[] =
    "_ZN15FProgramKeyData26ES2ShaderProgramKeyFields1E";
constexpr char kProgramKeyThunkName[] = "horn_fast_get_packed_program_key";

constexpr uint32_t kArmLdrPcLiteral = 0xE51FF004u;  // ldr pc,[pc,#-4]
constexpr std::array<uint32_t, 2> kGetPackedProgramKeySignature = {
    0xE59FC144u,  // ldr ip,[pc,#0x144]
    0xE92D0FF0u,  // push {r4-r11}
};

constexpr size_t kProgramKeyBytes = 16;
constexpr size_t kProgramKeyDataBytes = 0x183;
constexpr size_t kSecondFieldDataOffset = 193;

// These are the first DWORDs of Horn's two eight-byte field descriptors. The
// second DWORD is a value bound and is irrelevant to packing. Field 1 in the
// second group is the high-byte continuation of the ten-bit field 0.
constexpr std::array<uint8_t, 36> kFieldWidths0 = {
    2, 3, 1, 1, 2, 1, 1, 1, 1, 1, 3, 2,
    2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 4, 3, 1, 1, 1, 5, 5, 1, 2, 5, 2,
};
constexpr std::array<uint8_t, 19> kFieldWidths1 = {
    10, 0, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 5, 4, 1,
};

constexpr size_t SumWidths(const auto& widths) {
    size_t sum = 0;
    for (uint8_t width : widths) sum += width;
    return sum;
}
static_assert(SumWidths(kFieldWidths0) == 64);
static_assert(SumWidths(kFieldWidths1) == 35);

bool IsGuestRangeValid(const DynarmicHost& host, uint32_t address,
                       size_t size) {
    return address != 0 &&
           static_cast<uint64_t>(address) + size <= host.GetGuestMemorySize();
}

template <size_t Count>
uint64_t PackFieldGroup(const uint8_t* values,
                        const std::array<uint8_t, Count>& widths) {
    uint64_t packed = 0;
    for (size_t index = 0; index < Count; ++index) {
        const uint32_t width = widths[index];
        const uint32_t mask = width == 0 ? 0u : (1u << width) - 1u;
        packed = (packed << width) + (values[index] & mask);
        if (width > 8 && index + 1 < Count) {
            packed += static_cast<uint64_t>(values[index + 1]) << 8;
        }
    }
    return packed;
}

void PackProgramKeyData(const uint8_t* data, uint8_t* output) {
    // Match the guest routine's observable ordering in case a caller ever
    // aliases the output and source buffers.
    std::memset(output, 0, kProgramKeyBytes);
    const uint64_t first = PackFieldGroup(data, kFieldWidths0);
    std::memcpy(output, &first, sizeof(first));
    const uint64_t second =
        PackFieldGroup(data + kSecondFieldDataOffset, kFieldWidths1);
    std::memcpy(output + sizeof(first), &second, sizeof(second));
}

template <size_t Count>
bool ValidateWidthTable(DynarmicHost& host, uint32_t address,
                        const std::array<uint8_t, Count>& expected) {
    if (!IsGuestRangeValid(host, address, Count * 8)) return false;
    for (size_t i = 0; i < Count; ++i) {
        if (host.MemoryRead32(address + static_cast<uint32_t>(i * 8)) !=
            expected[i]) {
            LOGE("Program-key width table mismatch at %zu: got %u expected %u",
                 i,
                 host.MemoryRead32(address + static_cast<uint32_t>(i * 8)),
                 expected[i]);
            return false;
        }
    }
    return true;
}

bool DifferentialProgramKeySelfTest(DynarmicHost& host,
                                    uint32_t guest_function) {
    const uint32_t data_address = host.GuestAlloc(0x200, 8);
    const uint32_t output_address = host.GuestAlloc(kProgramKeyBytes, 8);
    if (!data_address || !output_address) {
        if (data_address) host.GuestFree(data_address);
        if (output_address) host.GuestFree(output_address);
        LOGE("Program-key fast-path self-test allocation failed");
        return false;
    }

    uint8_t* const memory = host.GetGuestMemoryBase();
    auto* const data = memory + data_address;
    auto* const output = memory + output_address;
    bool passed = true;
    uint32_t random = 0xC001D00Du;
    for (uint32_t test = 0; test < 66 && passed; ++test) {
        std::memset(data, 0, 0x200);
        if (test == 1) {
            std::memset(data, 0xFF, kProgramKeyDataBytes);
        } else if (test >= 2) {
            for (size_t i = 0; i < kProgramKeyDataBytes; ++i) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                data[i] = static_cast<uint8_t>(random);
            }
        }

        std::array<uint8_t, kProgramKeyBytes> expected{};
        PackProgramKeyData(data, expected.data());
        std::memset(output, 0xA5, kProgramKeyBytes);
        host.RunFunction(guest_function, {data_address, output_address});
        if (std::memcmp(output, expected.data(), expected.size()) != 0) {
            LOGE("Program-key fast-path differential test %u failed", test);
            passed = false;
        }
    }

    host.GuestFree(output_address);
    host.GuestFree(data_address);
    return passed;
}

}  // namespace

bool InstallHornFastPaths(DynarmicHost& host) {
    const uint32_t function = host.FindSymbol(kGetPackedProgramKeySymbol);
    const uint32_t fields0 = host.FindSymbol(kProgramKeyFields0Symbol);
    const uint32_t fields1 = host.FindSymbol(kProgramKeyFields1Symbol);
    if (!function || !fields0 || !fields1) {
        LOGI("Program-key fast path unavailable; using original ARM32 routine");
        return false;
    }

    if (!IsGuestRangeValid(host, function, kGetPackedProgramKeySignature.size() * 4) ||
        host.MemoryRead32(function) != kGetPackedProgramKeySignature[0] ||
        host.MemoryRead32(function + 4) != kGetPackedProgramKeySignature[1]) {
        LOGE("Program-key fast path refused: function signature mismatch at 0x%08X",
             function);
        return false;
    }
    if (!ValidateWidthTable(host, fields0, kFieldWidths0) ||
        !ValidateWidthTable(host, fields1, kFieldWidths1) ||
        !DifferentialProgramKeySelfTest(host, function)) {
        LOGE("Program-key fast path refused: semantic validation failed");
        return false;
    }

    const uint32_t thunk = host.GetThunkManager().RegisterThunk(
        kProgramKeyThunkName,
        [](Dynarmic::A32::Jit& jit, DynarmicHost& thunk_host) {
            const uint32_t data_address = jit.Regs()[0];
            const uint32_t output_address = jit.Regs()[1];
            if (!IsGuestRangeValid(thunk_host, data_address, kProgramKeyDataBytes) ||
                !IsGuestRangeValid(thunk_host, output_address, kProgramKeyBytes)) {
                LOGE("Invalid program-key buffers: data=0x%08X output=0x%08X",
                     data_address, output_address);
                return;
            }
            uint8_t* const memory = thunk_host.GetGuestMemoryBase();
            PackProgramKeyData(memory + data_address, memory + output_address);
            // The original void routine happens to leave r0 one-past the
            // sixteen-byte output after its final post-indexed store.
            jit.Regs()[0] = output_address + kProgramKeyBytes;
        });
    if (!thunk) {
        LOGE("Program-key fast path refused: thunk allocation failed");
        return false;
    }

    host.MemoryWrite32(function, kArmLdrPcLiteral);
    host.MemoryWrite32(function + 4, thunk);
    host.InvalidateGuestCode(function, 8);
    if (host.MemoryRead32(function) != kArmLdrPcLiteral ||
        host.MemoryRead32(function + 4) != thunk) {
        LOGE("Program-key fast-path patch verification failed");
        return false;
    }

    LOGI("Program-key fast path active at 0x%08X -> thunk 0x%08X "
         "(66-case differential test passed)",
         function, thunk);
    return true;
}

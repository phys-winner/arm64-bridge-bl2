#include "bl2_vertex_element_fastpath.h"

#include "bridge_log.h"
#include "dynarmic_host.h"
#include "thunk_manager.h"

#include <dynarmic/interface/A32/a32.h>
#include <sys/system_properties.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define LOG_TAG "Arm32Bridge_VertexFast"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

namespace {

constexpr char kSymbol[] =
    "_ZN17FOpenGLDynamicRHI19EnableVertexElementERK19OpenGLVertexElementjiPvjyjj";
constexpr uint32_t kOriginalPush = 0xE92D43F8u;
constexpr uint32_t kOriginalLoadIndex = 0xE591C008u;
constexpr uint32_t kArmLdrPcLiteral = 0xE51FF004u;
constexpr size_t kObjectBytes = 0x1000;
constexpr size_t kElementBytes = 0x18;

bool Valid(const DynarmicHost& host, uint32_t address, size_t length) {
    return address != 0 && static_cast<uint64_t>(address) + length <=
           host.GetGuestMemorySize();
}

bool Overlaps(uint32_t first, size_t first_size,
              uint32_t second, size_t second_size) {
    return static_cast<uint64_t>(first) <
               static_cast<uint64_t>(second) + second_size &&
           static_cast<uint64_t>(second) <
               static_cast<uint64_t>(first) + first_size;
}

uint32_t Read32(const uint8_t* pointer) {
    uint32_t value;
    std::memcpy(&value, pointer, sizeof(value));
    return value;
}

void Write32(uint8_t* pointer, uint32_t value) {
    std::memcpy(pointer, &value, sizeof(value));
}

bool UnchangedVertexElement(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    const uint32_t object_address = jit.Regs()[0];
    const uint32_t element_address = jit.Regs()[1];
    const uint32_t stack_address = jit.Regs()[13];
    if (stack_address < 32 ||
        !Valid(host, element_address, kElementBytes) ||
        !Valid(host, stack_address - 32, 40)) {
        return false;
    }
    const uint8_t* const memory = host.GetGuestMemoryBase();
    const uint8_t* const element = memory + element_address;
    const uint32_t index = Read32(element + 8);
    if (index > 15 || !Valid(host, object_address, kObjectBytes)) {
        return false;
    }
    const uint8_t* const slot = memory + object_address + 0x318 + index * 36;
    if (Read32(slot) == 0) return false;
    const uint32_t arg4 = Read32(memory + stack_address);
    const uint32_t arg5 = Read32(memory + stack_address + 4);
    const uint32_t effective_arg2 = jit.Regs()[3] == 0 ? 8192u : jit.Regs()[2];
    return Read32(slot + 0x1c) == arg4 &&
           Read32(slot + 4) == arg5 &&
           Read32(slot + 8) == index &&
           Read32(slot + 0x0c) == Read32(element + 0x10) &&
           Read32(slot + 0x10) == Read32(element) &&
           slot[0x14] == element[0x14] &&
           Read32(slot + 0x18) == jit.Regs()[3] &&
           Read32(slot + 0x20) == effective_arg2;
}

void UpdateFlag(uint8_t* object, uint32_t index, uint32_t lane, bool set) {
    const uint32_t bit = 96 + index * 4 + lane;
    uint8_t* const word_pointer = object + 0x7a4 + (bit >> 5) * 4;
    uint32_t word = Read32(word_pointer);
    const uint32_t mask = 0x80000000u >> (bit & 31);
    word = set ? word | mask : word & ~mask;
    Write32(word_pointer, word);
}

bool NativeVertexElement(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    const uint32_t object_address = jit.Regs()[0];
    const uint32_t element_address = jit.Regs()[1];
    const uint32_t stack_address = jit.Regs()[13];
    if (stack_address < 32 ||
        !Valid(host, object_address, kObjectBytes) ||
        !Valid(host, element_address, kElementBytes) ||
        !Valid(host, stack_address - 32, 40) ||
        Overlaps(object_address, kObjectBytes,
                 element_address, kElementBytes) ||
        Overlaps(object_address, kObjectBytes, stack_address - 32, 40) ||
        Overlaps(element_address, kElementBytes, stack_address - 32, 40)) {
        return false;
    }
    uint8_t* const memory = host.GetGuestMemoryBase();
    uint8_t* const object = memory + object_address;
    const uint8_t* const element = memory + element_address;
    const uint32_t index = Read32(element + 8);
    if (index > 15) return false;
    uint8_t* const slot = object + 0x318 + index * 36;
    const uint32_t arg4 = Read32(memory + stack_address);
    const uint32_t arg5 = Read32(memory + stack_address + 4);
    if (Read32(slot) == 0) {
        Write32(slot, 1);
        UpdateFlag(object, index, 1, false);
        UpdateFlag(object, index, 0, true);
    }
    const uint32_t element_type = Read32(element);
    const uint32_t element_stride = Read32(element + 0x10);
    const uint8_t element_normalized = element[0x14];
    if (Read32(slot + 0x1c) != arg4 ||
        Read32(slot + 4) != arg5 ||
        Read32(slot + 8) != index ||
        Read32(slot + 0x0c) != element_stride ||
        Read32(slot + 0x10) != element_type ||
        slot[0x14] != element_normalized ||
        Read32(slot + 0x18) != jit.Regs()[3]) {
        Write32(slot + 0x1c, arg4);
        Write32(slot + 4, arg5);
        Write32(slot + 8, index);
        Write32(slot + 0x0c, element_stride);
        Write32(slot + 0x10, element_type);
        slot[0x14] = element_normalized;
        Write32(slot + 0x18, jit.Regs()[3]);
        UpdateFlag(object, index, 2, true);
    }
    const uint32_t effective_arg2 =
        jit.Regs()[3] == 0 ? 8192u : jit.Regs()[2];
    if (Read32(slot + 0x20) != effective_arg2) {
        Write32(slot + 0x20, effective_arg2);
        UpdateFlag(object, index, 3, true);
    }
    jit.Regs()[2] = effective_arg2;
    return true;
}

void ResumeOriginalAfterPrologue(Dynarmic::A32::Jit& jit,
                                 DynarmicHost& host,
                                 uint32_t function_address) {
    // Restore the two overwritten ARM instructions before continuing at +8:
    // push {r3-r9,lr}; ldr r12,[r1,#8]. This fallback executes the complete
    // original state-change path without repatching shared guest code.
    const uint32_t original_sp = jit.Regs()[13];
    const uint32_t element_address = jit.Regs()[1];
    if (original_sp < 32 || !Valid(host, original_sp - 32, 32) ||
        !Valid(host, element_address, 12)) {
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    uint8_t* const memory = host.GetGuestMemoryBase();
    const uint32_t stack = original_sp - 32;
    for (uint32_t reg = 3; reg <= 9; ++reg) {
        Write32(memory + stack + (reg - 3) * 4, jit.Regs()[reg]);
    }
    Write32(memory + stack + 28, jit.Regs()[14]);
    jit.Regs()[13] = stack;
    jit.Regs()[12] = Read32(memory + element_address + 8);
    jit.Regs()[15] = function_address + 8;
}

struct TestCase {
    std::array<uint8_t, kObjectBytes> initial{};
    std::array<uint8_t, kObjectBytes> expected{};
    std::array<uint8_t, kElementBytes> element{};
    std::vector<uint32_t> args;
    uint32_t expected_result = 0;
};

std::vector<TestCase> MakeTestCases(uint32_t object_address,
                                    uint32_t element_address) {
    std::vector<TestCase> cases(112);
    uint32_t random = 0x734E2A19u;
    for (uint32_t test = 0; test < cases.size(); ++test) {
        TestCase& current = cases[test];
        for (uint8_t& byte : current.initial) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            byte = static_cast<uint8_t>(random);
        }
        const uint32_t index = test % 16;
        const uint32_t mode = test / 16;
        const uint32_t arg2 = 0x80000000u | test;
        const uint32_t arg3 = mode == 5 ? 0 : (0x10000u + test);
        const uint32_t arg4 = 0x20000u + test;
        const uint32_t arg5 = 0x30000u + test;
        const uint32_t effective_arg2 = arg3 == 0 ? 8192u : arg2;
        Write32(current.element.data(), 0x40000u + test);
        Write32(current.element.data() + 8, index);
        Write32(current.element.data() + 0x10, 0x50000u + test);
        current.element[0x14] = static_cast<uint8_t>(test);
        uint8_t* const slot = current.initial.data() + 0x318 + index * 36;
        Write32(slot, mode == 1 ? 0 : 1);
        Write32(slot + 4, arg5);
        Write32(slot + 8, index);
        Write32(slot + 0x0c, Read32(current.element.data() + 0x10));
        Write32(slot + 0x10, Read32(current.element.data()));
        slot[0x14] = current.element[0x14];
        Write32(slot + 0x18, arg3);
        Write32(slot + 0x1c, arg4);
        Write32(slot + 0x20, effective_arg2);
        if (mode == 2) Write32(slot + 0x1c, arg4 ^ 1u);
        if (mode == 3) Write32(slot + 0x10,
                               Read32(current.element.data()) ^ 1u);
        if (mode == 4) Write32(slot + 0x20, effective_arg2 ^ 1u);
        const uint32_t effective_element_address = mode == 6
            ? object_address + 0x600 : element_address;
        if (mode == 6) {
            std::memcpy(current.initial.data() + 0x600,
                        current.element.data(), current.element.size());
        }
        current.args = {object_address, effective_element_address,
                        arg2, arg3, arg4,
                        arg5, 0, 0, 0, 0};
    }
    return cases;
}

bool RunDifferentialCases(DynarmicHost& host, uint32_t function_address,
                          uint32_t object_address, uint32_t element_address,
                          std::vector<TestCase>& cases, bool patched) {
    uint8_t* const memory = host.GetGuestMemoryBase();
    size_t void_return_mismatches = 0;
    for (size_t test = 0; test < cases.size(); ++test) {
        TestCase& current = cases[test];
        std::memcpy(memory + object_address, current.initial.data(),
                    current.initial.size());
        std::memcpy(memory + element_address, current.element.data(),
                    current.element.size());
        const uint32_t result = host.RunFunction(function_address, current.args);
        if (!patched) {
            current.expected_result = result;
            std::memcpy(current.expected.data(), memory + object_address,
                        current.expected.size());
        } else {
            // The guest function returns void. ARM r0 is caller-saved and may
            // contain a scratch address after the final dirty-bit update.
            void_return_mismatches += result != current.expected_result;
            for (size_t offset = 0; offset < current.expected.size(); ++offset) {
                if (memory[object_address + offset] != current.expected[offset]) {
                    LOGE("Vertex differential test %zu failed (mode=%zu) "
                         "offset=0x%zX actual=%02X expected=%02X "
                         "r0=%08X expected_r0=%08X",
                         test, test / 16, offset,
                         memory[object_address + offset],
                         current.expected[offset], result,
                         current.expected_result);
                    return false;
                }
            }
        }
    }
    if (patched) {
        LOGI("Vertex differential memory matches: %zu cases, void r0 "
             "scratch differences=%zu", cases.size(), void_return_mismatches);
    }
    return true;
}

}  // namespace

bool InstallBl2VertexElementFastpath(DynarmicHost& host) {
    char property[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2.vertex_fastpath", property) <= 0 ||
        (property[0] != '1' && property[0] != '2')) {
        return true;
    }
    const bool full_native = property[0] == '2';
    const uint32_t function = host.FindSymbol(kSymbol);
    if (!function || !Valid(host, function, 8) ||
        host.MemoryRead32(function) != kOriginalPush ||
        host.MemoryRead32(function + 4) != kOriginalLoadIndex) {
        LOGE("Vertex fast path refused: input signature mismatch at 0x%08X",
             function);
        return false;
    }
    const uint32_t object_address = host.GuestAlloc(kObjectBytes, 16);
    const uint32_t element_address = host.GuestAlloc(kElementBytes, 16);
    if (!object_address || !element_address) {
        if (element_address) host.GuestFree(element_address);
        if (object_address) host.GuestFree(object_address);
        LOGE("Vertex fast path refused: self-test allocation failed");
        return false;
    }
    std::vector<TestCase> cases = MakeTestCases(object_address, element_address);
    const bool original_ok = RunDifferentialCases(
        host, function, object_address, element_address, cases, false);
    if (!original_ok) {
        host.GuestFree(element_address);
        host.GuestFree(object_address);
        LOGE("Vertex fast path refused: original guest self-test failed");
        return false;
    }
    const uint32_t thunk = host.GetThunkManager().RegisterThunk(
        "bl2_vertex_element_fastpath",
        [function, full_native](Dynarmic::A32::Jit& jit,
                                DynarmicHost& thunk_host) {
            static thread_local uint32_t calls = 0;
            static thread_local uint32_t hits = 0;
            const bool hit = full_native
                ? NativeVertexElement(jit, thunk_host)
                : UnchangedVertexElement(jit, thunk_host);
            hits += hit;
            if (!hit) ResumeOriginalAfterPrologue(jit, thunk_host, function);
            if (++calls == 1'000'000) {
                LOGI("Vertex fast path profile calls=%u hits=%u fallback=%u",
                     calls, hits, calls - hits);
                calls = hits = 0;
            }
        });
    if (!thunk) {
        host.GuestFree(element_address);
        host.GuestFree(object_address);
        return false;
    }
    host.MemoryWrite32(function, kArmLdrPcLiteral);
    host.MemoryWrite32(function + 4, thunk);
    host.InvalidateGuestCode(function, 8);
    const bool passed = host.MemoryRead32(function) == kArmLdrPcLiteral &&
                        host.MemoryRead32(function + 4) == thunk &&
                        RunDifferentialCases(host, function, object_address,
                                             element_address, cases, true);
    host.GuestFree(element_address);
    host.GuestFree(object_address);
    if (!passed) {
        host.MemoryWrite32(function, kOriginalPush);
        host.MemoryWrite32(function + 4, kOriginalLoadIndex);
        host.InvalidateGuestCode(function, 8);
        LOGE("Vertex fast path refused: patch/differential validation failed; "
             "original restored");
        return false;
    }
    LOGI("Vertex fast path active: guest=0x%08X thunk=0x%08X "
         "mode=%s (112-case differential test passed)",
         function, thunk, full_native ? "native" : "unchanged-only");
    return true;
}

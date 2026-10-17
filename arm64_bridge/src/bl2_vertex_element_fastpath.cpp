#include "guest_software_write_watch.h"
#include "bl2_vertex_element_fastpath.h"

#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "thunk_manager.h"

#include <dynarmic/interface/A32/a32.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <array>
#include <atomic>
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

uint32_t NativeVertexElement(uint8_t* object, const uint8_t* element,
                             uint32_t arg2, uint32_t arg3,
                             uint32_t arg4, uint32_t arg5) {
    const uint32_t index = Read32(element + 8);
    uint8_t* const slot = object + 0x318 + index * 36;
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
        Read32(slot + 0x18) != arg3) {
        Write32(slot + 0x1c, arg4);
        Write32(slot + 4, arg5);
        Write32(slot + 8, index);
        Write32(slot + 0x0c, element_stride);
        Write32(slot + 0x10, element_type);
        slot[0x14] = element_normalized;
        Write32(slot + 0x18, arg3);
        UpdateFlag(object, index, 2, true);
    }
    const uint32_t effective_arg2 = arg3 == 0 ? 8192u : arg2;
    if (Read32(slot + 0x20) != effective_arg2) {
        Write32(slot + 0x20, effective_arg2);
        UpdateFlag(object, index, 3, true);
    }
    return effective_arg2;
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
    if (Read32(memory + element_address + 8) > 15) return false;
    jit.Regs()[2] = NativeVertexElement(
        memory + object_address, memory + element_address, jit.Regs()[2],
        jit.Regs()[3], Read32(memory + stack_address),
        Read32(memory + stack_address + 4));
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
        NotifyGuestMemoryWritten(object_address, current.initial.size());
        NotifyGuestMemoryWritten(element_address, current.element.size());
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

constexpr char kSetupSymbol[] =
    "_ZN17FOpenGLDynamicRHI17SetupVertexArraysEjP13TOpenGLBufferIL17ERHIResourceTypes10ELj34963EXadL_Z28CachedBindElementArrayBufferjjEEE";
// Complete 492-byte original, including both PC-relative literals. SHA256:
// 715E3FEF457158103EF54B8F5AC0A9B68FD26CA1F57E36FEA46F1D827198FA0A.
// No ELF relocation targets this span. Exact comparison also guards the leaf
// call, declaration layout, dirty-bit operations and the global EAB location.
constexpr std::array<uint32_t, 123> kSetupOriginal = {
    0xE92D4FF0u, 0xE3520000u, 0xE59038A4u, 0xE24DD024u, 0xE1A04000u, 0xE58D201Cu,
    0x15922008u, 0xE58D1018u, 0xE3530000u, 0x1A000002u, 0xE59037A0u, 0xE3530000u,
    0x0A00005Au, 0xE594375Cu, 0xE593A02Cu, 0xE59A300Cu, 0xE3530000u, 0xDA00005Du,
    0xE3A08000u, 0xE1A06008u, 0xE1A07008u, 0xE59A5008u, 0xE1A00004u, 0xE0855008u,
    0xE2888018u, 0xE5952004u, 0xE1A01005u, 0xE595900Cu, 0xE1A02202u, 0xE2822F86u,
    0xE0843002u, 0xE794C002u, 0xE593B008u, 0xE593E004u, 0xE593200Cu, 0xE089B00Bu,
    0xE59D9018u, 0xE1A0300Eu, 0xE02EB99Eu, 0xE58DE000u, 0xE59CE008u, 0xE58DE004u,
    0xEDDC0B04u, 0xEDCD0B02u, 0xE59CC020u, 0xE58D6014u, 0xE2866001u, 0xE58DC010u,
    0xEBFFFF42u, 0xE59A200Cu, 0xE3A01001u, 0xE5953008u, 0xE1520006u, 0xE1877311u,
    0xCAFFFFDDu, 0xE1A00004u, 0xE3A01061u, 0xE3A02060u, 0xE3A03000u, 0xE3A0A102u,
    0xE1A0C337u, 0xE2833001u, 0xE21CC001u, 0x1A00000Fu, 0xE5908318u, 0xE1A062A2u,
    0xE1A052A1u, 0xE202901Cu, 0xE0846106u, 0xE0845105u, 0xE3580000u, 0xE201B01Fu,
    0x0A000006u, 0xE580C318u, 0xE596C7A4u, 0xE1CC993Au, 0xE58697A4u, 0xE595C7A4u,
    0xE18CBB3Au, 0xE585B7A4u, 0xE3530010u, 0xE2800024u, 0xE2822004u, 0xE2811004u,
    0x1AFFFFE6u, 0xE59D901Cu, 0xE3590000u, 0x0A000019u, 0xE59F207Cu, 0xE5993008u,
    0xE08F2002u, 0xE5922014u, 0xE1520003u, 0x0A000003u, 0xE59427A8u, 0xE5843558u,
    0xE3823101u, 0xE58437A8u, 0xE59D9018u, 0xE3A03000u, 0xE58438A4u, 0xE58498A8u,
    0xE28DD024u, 0xE8BD8FF0u, 0xE5903558u, 0xE1520003u, 0x1AFFFFA1u, 0xE59038A8u,
    0xE59D9018u, 0xE1530009u, 0x1AFFFF9Du, 0xEAFFFFF5u, 0xE3A07000u, 0xEAFFFFC4u,
    0xE59F3018u, 0xE08F3003u, 0xE5933014u, 0xE3530000u, 0x159D301Cu, 0x1AFFFFE5u,
    0xEAFFFFE8u, 0x012278A4u, 0x01227840u,
};
std::atomic<bool> g_setup_enabled{false};
std::atomic<bool> g_setup_runtime_trace_pending{false};

bool SetupSafeRange(DynarmicHost& host, uint32_t address, size_t size,
                     bool writable = false) {
    if (!address || (address & 3) || !size ||
        static_cast<uint64_t>(address) + size > host.GetGuestMemorySize()) {
        return false;
    }
    const uint64_t end = static_cast<uint64_t>(address) + size;
    // Anonymous allocator and guest stacks are permanently RW. File-backed
    // mappings and reserved address-space holes use the original ARM path.
    if (DynarmicHost::IsHeapRange(address, size)) return true;
    auto& loader = host.GetElfLoader();
    const uint64_t base = loader.GetBaseAddress();
    if (address >= base + 0x02d209d0u && end <= base + 0x03088fb8u) {
        return true;
    }
    return !writable && loader.IsReadOnlyRange(address, size);
}

bool NativeSetupVertexArrays(DynarmicHost& host, uint32_t object_address,
                             uint32_t vertex_offset, uint32_t index_buffer,
                             uint32_t stack_address, uint32_t cached_eab) {
    // The original saves 36 bytes of registers plus 36 bytes of locals. Any
    // source inside that frame, or inside RHI, can change before a later read.
    if (stack_address < 72 ||
        !SetupSafeRange(host, stack_address - 72, 72, true) ||
        !SetupSafeRange(host, object_address, kObjectBytes, true) ||
        Overlaps(object_address, kObjectBytes, stack_address - 72, 72)) {
        return false;
    }
    const auto source_ok = [&](uint32_t address, size_t bytes) {
        return SetupSafeRange(host, address, bytes) &&
               !Overlaps(address, bytes, object_address, kObjectBytes) &&
               !Overlaps(address, bytes, stack_address - 72, 72);
    };
    uint8_t* const memory = host.GetGuestMemoryBase();
    uint8_t* const object = memory + object_address;
    if (index_buffer && !source_ok(index_buffer, 12)) return false;
    const uint32_t index_name = index_buffer
        ? Read32(memory + index_buffer + 8) : 0;
    if (Read32(object + 0x8a4) == 0 && Read32(object + 0x7a0) == 0 &&
        Read32(object + 0x558) == index_name &&
        Read32(object + 0x8a8) == vertex_offset) {
        return true;
    }
    const uint32_t bound_shader = Read32(object + 0x75c);
    if (!source_ok(bound_shader, 0x30) || !source_ok(cached_eab, 4)) {
        return false;
    }
    const uint32_t declaration = Read32(memory + bound_shader + 0x2c);
    if (!source_ok(declaration, 16)) return false;
    const int32_t count = static_cast<int32_t>(
        Read32(memory + declaration + 12));
    // Sixteen is the supported attribute domain, not an invented guest limit:
    // larger (including duplicate) declarations execute the saved original.
    if (count > 16) return false;
    const uint32_t elements = Read32(memory + declaration + 8);
    if (count > 0 && !source_ok(elements,
                               static_cast<size_t>(count) * kElementBytes)) {
        return false;
    }
    struct ElementArgs {
        const uint8_t* element;
        uint32_t instance, stride, offset, buffer;
    };
    std::array<ElementArgs, 16> args;
    uint32_t enabled = 0;
    // Preflight every future read before the first mutation. In particular,
    // a later invalid stream must never leave earlier attributes half updated.
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* const element = memory + elements + i * kElementBytes;
        const uint32_t stream_index = Read32(element + 4);
        const uint32_t attribute_index = Read32(element + 8);
        if (stream_index > 15 || attribute_index > 15) return false;
        const uint8_t* const stream = object + 0x218 + stream_index * 16;
        const uint32_t buffer = Read32(stream);
        if (!source_ok(buffer, 0x24)) return false;
        const uint32_t stride = Read32(stream + 4);
        args[i] = {element, Read32(stream + 12), stride,
                   Read32(element + 12) + Read32(stream + 8) +
                       vertex_offset * stride,
                   Read32(memory + buffer + 8)};
        enabled |= 1u << attribute_index;
    }
    for (int32_t i = 0; i < count; ++i) {
        const ElementArgs& current = args[i];
        NativeVertexElement(object, current.element, current.instance,
                            current.stride, current.offset, current.buffer);
    }
    for (uint32_t index = 0; index < 16; ++index) {
        uint8_t* const slot = object + 0x318 + index * 36;
        if (!(enabled & (1u << index)) && Read32(slot) != 0) {
            Write32(slot, 0);
            UpdateFlag(object, index, 0, false);
            UpdateFlag(object, index, 1, true);
        }
    }
    // The original compares the actual GL binding cache, not object+0x558.
    if (index_name != Read32(memory + cached_eab)) {
        Write32(object + 0x558, index_name);
        Write32(object + 0x7a8, Read32(object + 0x7a8) | 0x40000000u);
    }
    Write32(object + 0x8a4, 0);
    Write32(object + 0x8a8, vertex_offset);
    NotifyGuestMemoryWritten(object_address + 0x318, 0x8ac - 0x318);
    return true;
}

constexpr size_t kSetupFixtureBytes = 0x2000;
constexpr uint32_t kSetupAbiOffset = 0x1900;
constexpr size_t kSetupAbiOutputBytes = 104;

struct SetupCase {
    std::array<uint8_t, kSetupFixtureBytes> initial{};
    std::array<uint8_t, kSetupFixtureBytes> expected{};
    uint32_t vertex_offset = 0, index_buffer = 0, cached_eab = 0;
    bool native_expected = true;
};

std::vector<SetupCase> MakeSetupCases(uint32_t fixture) {
    std::vector<SetupCase> cases(52);
    uint32_t random = 0xc65709b3u;
    for (size_t test = 0; test < cases.size(); ++test) {
        SetupCase& current = cases[test];
        for (uint8_t& byte : current.initial) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            byte = static_cast<uint8_t>(random);
        }
        uint8_t* const data = current.initial.data();
        const uint32_t mode = static_cast<uint32_t>(test % 26);
        uint32_t shader = 0x1000, declaration = 0x1040, elements = 0x1080;
        int32_t count = 4;
        if (mode == 5) count = 0;
        if (mode == 6) count = -7;
        if (mode == 7 || mode == 8 || mode == 20) count = 1;
        if (mode == 9 || mode == 21) count = 16;
        if (mode == 10 || mode == 25) count = 17;
        if (mode == 11 || mode == 25) elements = 0x900;
        if (mode == 12) shader = 0xf00;
        if (mode == 13) declaration = 0xf40;
        current.native_expected = !(mode >= 10 && mode <= 15) && mode != 25;
        current.vertex_offset = mode == 23 ? 0xfffffffeu
            : 0x100u + static_cast<uint32_t>(test);
        current.index_buffer = (mode == 16 || mode == 17) ? 0
            : fixture + (mode == 15 ? 0xc80u : 0x1800u);
        const uint32_t index_name = mode == 24 ? 0u : 0x27100u;
        current.cached_eab = mode == 16 ? 0u :
            (mode == 18 ? index_name : index_name ^ 0x800u);
        Write32(data + 0x75c, fixture + shader);
        Write32(data + shader + 0x2c, fixture + declaration);
        Write32(data + declaration + 8, fixture + elements);
        Write32(data + declaration + 12, static_cast<uint32_t>(count));
        Write32(data + 0x8a4, 1);
        Write32(data + 0x7a0, mode == 2 ? 1u : 0u);
        Write32(data + 0x558, mode == 18 ? index_name ^ 1u : index_name);
        Write32(data + 0x8a8, current.vertex_offset);
        if (current.index_buffer) Write32(
            data + current.index_buffer - fixture + 8, index_name);
        for (uint32_t stream = 0; stream < 16; ++stream) {
            const uint32_t buffer = mode == 14 && stream == 0
                ? 0xc00u : 0x1400u + stream * 0x40u;
            Write32(data + buffer + 8, 0x47100u + stream);
            Write32(data + 0x218 + stream * 16, fixture + buffer);
            Write32(data + 0x21c + stream * 16,
                    mode == 23 || (mode == 20 && stream == 0) ? 0u
                    : 0x20u + stream * 4u);
            Write32(data + 0x220 + stream * 16, 0xffffffd0u + stream);
            Write32(data + 0x224 + stream * 16, 3u + stream);
            Write32(data + 0x318 + stream * 36,
                    mode == 21 ? 0u : (stream & 1u) + 1u);
        }
        for (int32_t i = 0; i < count; ++i) {
            uint8_t* const element = data + elements + i * kElementBytes;
            uint32_t index = static_cast<uint32_t>(i) & 15u;
            if (mode == 8) index = 15;
            if (mode == 22) index = i < 2 ? 0u : 15u;
            Write32(element, 0x1406u + static_cast<uint32_t>(i));
            Write32(element + 4, static_cast<uint32_t>(i) & 15u);
            Write32(element + 8, index);
            Write32(element + 12, 0xffffffc0u + static_cast<uint32_t>(i));
            Write32(element + 16, 1u + static_cast<uint32_t>(i));
            element[20] = static_cast<uint8_t>(0x80 + i);
        }
        if (mode == 20) {
            const uint8_t* const element = data + elements;
            uint8_t* const slot = data + 0x318;
            Write32(slot, 1);
            Write32(slot + 4, Read32(data + 0x1408));
            Write32(slot + 8, 0);
            Write32(slot + 12, Read32(element + 16));
            Write32(slot + 16, Read32(element));
            slot[20] = element[20];
            Write32(slot + 24, 0);
            Write32(slot + 28, Read32(element + 12) + Read32(data + 0x220));
            Write32(slot + 32, 8192);
        }
        if (mode >= 1 && mode <= 4) {
            Write32(data + 0x8a4, 0);
            if (mode == 1) Write32(data + 0x75c, 0);
            if (mode == 3) Write32(data + 0x8a8, current.vertex_offset ^ 1u);
            if (mode == 4) Write32(data + 0x558, index_name ^ 1u);
        }
    }
    return cases;
}

// Fixture wrapper captures callee-saved GPRs, D8-D15, SP and FPSCR after the
// real call. Both original and patched cases execute this identical ARM code.
bool WriteSetupTestWrapper(DynarmicHost& host, uint32_t wrapper,
                            uint32_t function) {
    std::vector<uint32_t> words = {0xe92d4ff0u, 0xe24dd004u, 0xe1a04003u};
    for (uint32_t reg = 5; reg <= 11; ++reg) {
        words.push_back(0xe3a00000u | (reg << 12) | (0x50u + reg));
    }
    for (uint32_t reg = 8; reg <= 15; ++reg) {
        words.push_back(0xed940b00u | (reg << 12) | (0x40u + (reg - 8) * 2));
    }
    const size_t call = words.size();
    words.push_back(0); // LDR r12, literal function.
    words.push_back(0xe12fff3cu);
    words.insert(words.end(), {0xe8840ff0u, 0xe1a0300du, 0xe5843020u,
                               0xeef13a10u, 0xe5843024u});
    for (uint32_t reg = 8; reg <= 15; ++reg) {
        words.push_back(0xed840b00u | (reg << 12) | (10u + (reg - 8) * 2));
    }
    words.insert(words.end(), {0xe28dd004u, 0xe8bd8ff0u});
    words[call] = 0xe59fc000u |
        static_cast<uint32_t>((words.size() - call - 2) * 4);
    words.push_back(function);
    if (!Valid(host, wrapper, words.size() * 4)) return false;
    for (size_t i = 0; i < words.size(); ++i) {
        host.MemoryWrite32(wrapper + static_cast<uint32_t>(i * 4), words[i]);
    }
    host.AddExecutableRange(wrapper, static_cast<uint32_t>(words.size() * 4));
    host.InvalidateGuestCode(wrapper, words.size() * 4);
    return true;
}

void ResetSetupCase(DynarmicHost& host, uint32_t fixture,
                    uint32_t cached_eab, const SetupCase& current) {
    std::memcpy(host.GetGuestMemoryBase() + fixture,
                current.initial.data(), current.initial.size());
    NotifyGuestMemoryWritten(fixture, current.initial.size());
    host.MemoryWrite32(cached_eab, current.cached_eab);
}

bool RunSetupCases(DynarmicHost& host, uint32_t function,
                   uint32_t fixture, uint32_t cached_eab,
                   std::vector<SetupCase>& cases, bool record) {
    uint8_t* const memory = host.GetGuestMemoryBase();
    for (size_t i = 0; i < cases.size(); ++i) {
        SetupCase& current = cases[i];
        ResetSetupCase(host, fixture, cached_eab, current);
        host.RunFunction(function, {fixture, current.vertex_offset,
                                    current.index_buffer, fixture + kSetupAbiOffset});
        if (record) {
            std::memcpy(current.expected.data(), memory + fixture,
                        current.expected.size());
        } else if (std::memcmp(memory + fixture, current.expected.data(),
                               current.expected.size()) != 0) {
            LOGE("SetupVertexArrays differential failed: case=%zu enabled=%d",
                 i, g_setup_enabled.load(std::memory_order_relaxed));
            return false;
        }
        if (Read32(memory + cached_eab) != current.cached_eab) return false;
        // The wrapper's r4 output pointer, r5-r11 sentinels and D8-D15 must
        // survive the original call as required by the ARM softfp ABI.
        const uint8_t* const abi = memory + fixture + kSetupAbiOffset;
        if (Read32(abi) != fixture + kSetupAbiOffset ||
            std::memcmp(abi + 40, abi + 0x100, 64) != 0) return false;
        for (uint32_t reg = 5; reg <= 11; ++reg) {
            if (Read32(abi + (reg - 4) * 4) != 0x50u + reg) return false;
        }
    }
    return true;
}

bool VerifyNativeSetupCases(DynarmicHost& host, uint32_t fixture,
                            uint32_t cached_eab,
                            const std::vector<SetupCase>& cases) {
    uint8_t* const memory = host.GetGuestMemoryBase();
    for (size_t i = 0; i < cases.size(); ++i) {
        const SetupCase& current = cases[i];
        ResetSetupCase(host, fixture, cached_eab, current);
        const bool handled = NativeSetupVertexArrays(host, fixture,
            current.vertex_offset, current.index_buffer, fixture + 0x1f00, cached_eab);
        if (handled != current.native_expected) return false;
        const uint8_t* const expected = handled
            ? current.expected.data() : current.initial.data();
        for (size_t offset = 0; offset < kSetupFixtureBytes; ++offset) {
            if (handled && offset >= kSetupAbiOffset &&
                offset < kSetupAbiOffset + kSetupAbiOutputBytes) continue;
            if (memory[fixture + offset] != expected[offset]) {
                LOGE("SetupVertexArrays native mismatch: case=%zu offset=%zX",
                     i, offset);
                return false;
            }
        }
        if (Read32(memory + cached_eab) != current.cached_eab) return false;
    }
    // Inputs rejected before mutation: overflow, attribute/stream bounds,
    // invalid later element/VB and sources overwritten by the guest stack.
    for (uint32_t test = 0; test < 12; ++test) {
        ResetSetupCase(host, fixture, cached_eab, cases.front());
        uint32_t object = fixture, ib = cases.front().index_buffer;
        uint32_t stack = fixture + 0x1f00;
        if (test == 0) object = 0xfffffff0u;
        if (test == 1) ib = 0xfffffffcu;
        if (test == 2) Write32(memory + fixture + 0x1080 + 3 * 24 + 8, 16);
        if (test == 3) Write32(memory + fixture + 0x1080 + 3 * 24 + 4, 16);
        if (test == 4) Write32(memory + fixture + 0x218 + 3 * 16, 0);
        if (test == 5) stack = fixture + 0x1000;
        if (test == 6) Write32(memory + fixture + 0x1048, 0xfffffffcu);
        if (test == 7) stack = cached_eab + 4;
        if (test == 8) object = 0x80001000u;
        if (test == 9) ib = 0x80001000u;
        if (test == 10) Write32(memory + fixture + 0x1048, 0x80001000u);
        if (test == 11) Write32(memory + fixture + 0x75c, 0x80001000u);
        std::array<uint8_t, kSetupFixtureBytes> before;
        std::memcpy(before.data(), memory + fixture, before.size());
        if (NativeSetupVertexArrays(host, object, cases.front().vertex_offset,
                                    ib, stack, cached_eab) ||
            std::memcmp(memory + fixture, before.data(), before.size()) != 0 ||
            Read32(memory + cached_eab) != cases.front().cached_eab) return false;
    }
    return true;
}

void ReportSetupSelfTest(bool passed, const char* detail) {
    char property[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2.perf_tests", property) == 1 &&
        property[0] == '1') {
        __android_log_print(ANDROID_LOG_INFO, "BL2PerfCheck",
            "SetupVertexArrays %s original/patched/disabled=52 native=38 "
            "saved-fallback=14 guards=12 memory+callee-GPR/VFP/SP/FPSCR %s",
            passed ? "PASS" : "FAIL", detail);
    }
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

void SetBl2SetupVertexArraysEnabled(bool enabled) {
    g_setup_enabled.store(enabled, std::memory_order_relaxed);
}

bool InstallBl2SetupVertexArraysFastpath(DynarmicHost& host, bool startupEnabled) {
    char property[PROP_VALUE_MAX] = {};
    const int length = __system_property_get("debug.bl2.setup_arrays", property);
    if (length > 0) {
        if (length != 1 || (property[0] != '0' && property[0] != '1')) return true;
        startupEnabled = property[0] == '1';
    }
    if (!startupEnabled) return true;
    char trace_property[PROP_VALUE_MAX] = {};
    const bool runtime_trace =
        __system_property_get("debug.bl2.perf_tests", trace_property) == 1 &&
        trace_property[0] == '1';
    // Arm only after the last fixture call, so a self-test cannot masquerade
    // as a real rendering hit. Quiet startup avoids this flag on every call.
    g_setup_runtime_trace_pending.store(false, std::memory_order_relaxed);
    const uint32_t function = host.FindSymbol(kSetupSymbol);
    auto& loader = host.GetElfLoader();
    if (function != loader.GetBaseAddress() + 0x01e310ecu ||
        !loader.IsCodeRange(function, sizeof(kSetupOriginal)) ||
        !Valid(host, function, sizeof(kSetupOriginal)) ||
        std::memcmp(host.GetGuestMemoryBase() + function,
                    kSetupOriginal.data(), sizeof(kSetupOriginal)) != 0) {
        LOGE("SetupVertexArrays refused: complete original signature mismatch");
        ReportSetupSelfTest(false, "original-signature");
        return false;
    }
    // Original ADD at +0x168 uses PC+8 and the first literal at +0x1e4;
    // the global binding name is +0x14 within that cache object.
    const uint64_t cache = static_cast<uint64_t>(function) +
                           0x170u + kSetupOriginal[121] + 0x14u;
    if (cache > UINT32_MAX || !Valid(host, static_cast<uint32_t>(cache), 4)) {
        ReportSetupSelfTest(false, "global-cache-span");
        return false;
    }
    const uint32_t cached_eab = static_cast<uint32_t>(cache);
    const uint32_t saved_eab = host.MemoryRead32(cached_eab);
    const uint32_t fixture = host.GuestAlloc(kSetupFixtureBytes, 16);
    const uint32_t wrapper = host.GuestAlloc(160, 4);
    const uint32_t fallback = host.GuestAlloc(24, 4);
    const auto cleanup = [&](bool keep_fallback) {
        host.MemoryWrite32(cached_eab, saved_eab);
        if (!keep_fallback && fallback) host.GuestFree(fallback);
        if (wrapper) host.GuestFree(wrapper);
        if (fixture) host.GuestFree(fixture);
    };
    if (!fixture || !wrapper || !fallback ||
        !WriteSetupTestWrapper(host, wrapper, function)) {
        cleanup(false);
        ReportSetupSelfTest(false, "fixture-allocation");
        return false;
    }
    std::vector<SetupCase> cases = MakeSetupCases(fixture);
    if (!RunSetupCases(host, wrapper, fixture, cached_eab, cases, true) ||
        !VerifyNativeSetupCases(host, fixture, cached_eab, cases)) {
        cleanup(false);
        ReportSetupSelfTest(false, "original-native-differential");
        return false;
    }
    // Move only the first PUSH. Both veneers fetch ARM instructions normally;
    // their LDR-PC data literal must match the current guest data endianness.
    for (uint32_t offset : {0u, 12u}) {
        host.MemoryWrite32(fallback + offset, kSetupOriginal[0]);
        host.MemoryWrite32(fallback + offset + 4, kArmLdrPcLiteral);
        host.MemoryWrite32(fallback + offset + 8,
            offset ? __builtin_bswap32(function + 4) : function + 4);
    }
    host.AddExecutableRange(fallback, 24);
    host.InvalidateGuestCode(fallback, 24);
    const uint32_t thunk = host.GetThunkManager().RegisterThunk(
        "bl2_setup_arrays_fastpath",
        [fallback, cached_eab, runtime_trace](Dynarmic::A32::Jit& jit,
                                             DynarmicHost& thunk_host) {
            const uint32_t cpsr = jit.Cpsr();
            if ((cpsr & 0x0700fe20u) ||
                !g_setup_enabled.load(std::memory_order_relaxed) ||
                !NativeSetupVertexArrays(thunk_host, jit.Regs()[0], jit.Regs()[1],
                                         jit.Regs()[2], jit.Regs()[13], cached_eab)) {
                jit.Regs()[15] = fallback + ((cpsr & 0x200u) ? 12u : 0u);
                return;
            }
            if (runtime_trace &&
                g_setup_runtime_trace_pending.load(std::memory_order_relaxed) &&
                g_setup_runtime_trace_pending.exchange(false,
                                                      std::memory_order_relaxed)) {
                const uint32_t object = jit.Regs()[0];
                const uint8_t* const memory = thunk_host.GetGuestMemoryBase();
                const uint32_t shader = Read32(memory + object + 0x75c);
                uint32_t declaration = 0;
                int32_t count = 0;
                bool declaration_valid = false;
                // A cache-hit is allowed without a valid declaration. Probe
                // those optional fields only once and only in proven spans.
                if (SetupSafeRange(thunk_host, shader, 0x30)) {
                    declaration = Read32(memory + shader + 0x2c);
                    if (SetupSafeRange(thunk_host, declaration, 16)) {
                        count = static_cast<int32_t>(
                            Read32(memory + declaration + 12));
                        declaration_valid = true;
                    }
                }
                __android_log_print(ANDROID_LOG_INFO, "BL2PerfCheck",
                    "SetupVertexArrays runtime-hit object=%08X basevertex=%u "
                    "ib=%08X shader=%08X declaration=%08X count=%d "
                    "declaration-valid=%d EAB-global=%08X object-EAB=%08X",
                    object, jit.Regs()[1], jit.Regs()[2], shader, declaration,
                    count, declaration_valid, cached_eab,
                    Read32(memory + object + 0x558));
            }
            const uint32_t lr = jit.Regs()[14];
            jit.SetCpsr((cpsr & ~0x20u) | ((lr & 1u) << 5));
            jit.Regs()[15] = lr & ~1u;
        });
    if (!thunk) {
        cleanup(false);
        ReportSetupSelfTest(false, "thunk-registration");
        return false;
    }
    const uint32_t svc = host.MemoryRead32(thunk);
    if ((svc & 0xff000000u) != 0xef000000u || !(svc & 0x00ffffffu)) {
        cleanup(true); // Registered handler retains its fallback veneer.
        ReportSetupSelfTest(false, "svc-entry");
        return false;
    }
    // One SVC reaches the guarded handler even for BE data; LDR-PC+LE literal
    // would byte-swap the thunk address before that guard could run.
    host.MemoryWrite32(function, svc);
    host.InvalidateGuestCode(function, 4);
    SetBl2SetupVertexArraysEnabled(true);
    const bool native_passed = host.MemoryRead32(function) == svc &&
        host.MemoryRead32(function + 4) == kSetupOriginal[1] && RunSetupCases(
            host, wrapper, fixture, cached_eab, cases, false);
    SetBl2SetupVertexArraysEnabled(false);
    const bool disabled_passed = native_passed && RunSetupCases(
        host, wrapper, fixture, cached_eab, cases, false);
    if (!disabled_passed) {
        host.MemoryWrite32(function, kSetupOriginal[0]);
        host.InvalidateGuestCode(function, 4);
        cleanup(true); // Registered handler retains its fallback veneer.
        ReportSetupSelfTest(false, "patched-disabled-differential-original-restored");
        return false;
    }
    cleanup(true);
    SetBl2SetupVertexArraysEnabled(true);
    g_setup_runtime_trace_pending.store(runtime_trace, std::memory_order_relaxed);
    ReportSetupSelfTest(true, "startup-opt-in");
    LOGI("SetupVertexArrays candidate active: guest=0x%08X thunk=0x%08X "
         "52-case full-memory/ABI differential and disabled fallback passed",
         function, thunk);
    return true;
}

#include "bl2_audio_decode_fastpath.h"

#include "dynarmic_host.h"
#include "elf_loader.h"
#include "guest_software_write_watch.h"
#include "obb_sha256.h"
#include "thunk_manager.h"

#include <android/log.h>
#include <arm_neon.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#pragma STDC FENV_ACCESS ON

namespace {
constexpr char kDecodeSymbol[] = "_Z10decode_mapP8codebookP14oggpack_bufferPii";
constexpr char kFloorSymbol[] = "_Z15floor1_inverse2P16vorbis_dsp_stateP18vorbis_info_floor1PiS3_";
constexpr uint32_t kDecodeRva = 0x026DDEC8, kFloorRva = 0x026DBB98;
constexpr uint32_t kFloorTableRva = 0x02F7C73C;
constexpr uint32_t kLdrPc = 0xE51FF004;
constexpr size_t kFixtureBytes = 32768, kMaxFloorBins = 8192;
std::atomic<uint32_t> enabled{0};
uint32_t installed = 0, floor_table = 0;
uint32_t prepare_fpscr = 0;

template<class T> T Read(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(v)); return v; }
template<class T> void Write(uint8_t* p, T v) { std::memcpy(p, &v, sizeof(v)); }
bool Heap(const DynarmicHost& h, uint64_t p, size_t n) {
    return DynarmicHost::IsHeapRange(p, n) && p + n <= h.GetGuestMemorySize();
}
bool Overlap(uint64_t a, size_t n, uint64_t b, size_t m) {
    return n && m && a < b + m && b < a + n;
}
uint32_t Lsl(uint32_t v, uint32_t s) { s &= 255; return s < 32 ? v << s : 0; }
uint32_t Lsr(uint32_t v, uint32_t s) { s &= 255; return s < 32 ? v >> s : 0; }
uint32_t Asr(uint32_t v, uint32_t s) {
    s &= 255;
    if (!s) return v;
    if (s >= 32) return (v & 0x80000000u) ? UINT32_MAX : 0;
    return (v >> s) | ((v & 0x80000000u) ? UINT32_MAX << (32 - s) : 0);
}

bool Decode(DynarmicHost& h, const std::array<uint32_t,16>& r, uint32_t cpsr) {
    if (cpsr & 0x0700FE20u) return false;
    const uint32_t book = r[0], pack = r[1], out = r[2], scale = r[3], sp = r[13];
    if (!Heap(h, book, 60) || !Heap(h, pack, 12)) return false;
    auto* m = h.GetGuestMemoryBase(); const auto* b = m + book;
    const int32_t dimension = Read<int32_t>(b);
    const int32_t maxbits = Read<int32_t>(b + 12);
    if (dimension <= 0 || dimension > 256 || maxbits < 0 || maxbits > 32) return false;
    const size_t outbytes = size_t(dimension) * 4;
    if (!Heap(h, out, outbytes) || (out & 3) || sp < 40 || !Heap(h, sp - 40, 40) ||
        Overlap(out, outbytes, book, 60) || Overlap(out, outbytes, pack, 12) ||
        Overlap(pack, 12, book, 60) || Overlap(sp - 40, 40, book, 60) ||
        Overlap(sp - 40, 40, pack, 12) || Overlap(sp - 40, 40, out, outbytes)) return false;
    const auto readable = [&](uint64_t p, size_t n) {
        return Heap(h, p, n) && !Overlap(p, n, out, outbytes) &&
            !Overlap(p, n, pack, 12) && !Overlap(p, n, sp - 40, 40);
    };
    const uint32_t bit = Read<uint32_t>(m + pack), data = Read<uint32_t>(m + pack + 4);
    if (bit > 7) return false;
    const size_t lookbytes = std::max<size_t>(1, (maxbits + bit + 7) / 8);
    if (!readable(data, lookbytes)) return false;
    uint64_t peek64 = 0;
    for (size_t i = 0; i < lookbytes; ++i) peek64 |= uint64_t(m[data + i]) << (i * 8);
    uint32_t peek = uint32_t(peek64 >> bit) & (maxbits == 32 ? UINT32_MAX : (1u << maxbits) - 1);
    const uint32_t tree = Read<uint32_t>(b + 16), nodebytes = Read<uint32_t>(b + 20);
    const bool packed = Read<uint32_t>(b + 24) == 1;
    const uint32_t width = nodebytes == 1 ? 1 : nodebytes == 2 ? 2 : 4;
    const auto node = [&](uint32_t index, uint32_t& value) {
        const uint64_t p = uint64_t(tree) + uint64_t(index) * width;
        if (!readable(p, width)) return false;
        value = width == 1 ? m[p] : width == 2 ? Read<uint16_t>(m + p) : Read<uint32_t>(m + p);
        return true;
    };
    uint32_t payload = 0, consumed = 1;
    if (maxbits) {
        uint32_t index = peek & 1, previous = 0, direction = index, value;
        if (!node(index, value)) return false;
        const uint32_t leaf = width == 1 ? 0x80u : width == 2 ? 0x8000u : 0x80000000u;
        while (!(value & leaf)) {
            if (consumed >= uint32_t(maxbits)) return false;
            peek = Asr(peek, 1); direction = peek & 1; previous = value;
            index = (packed || width == 4) ? value * 2 + direction : value + direction;
            if (!node(index, value)) return false;
            ++consumed;
        }
        if (packed || width == 4) payload = value & (leaf - 1);
        else {
            uint32_t adjacent = 1;
            if (direction) {
                uint32_t prior;
                if (!node(previous, prior)) return false;
                adjacent = (prior & leaf) ? 1u : 0u;
            }
            uint32_t low;
            if (!node(index + adjacent + 1, low)) return false;
            payload = ((value << (width * 8)) | low) & (width == 1 ? 0x7FFFu : 0x7FFFFFFFu);
        }
    }
    std::array<uint32_t,256> output;
    const uint32_t qbits = Read<uint32_t>(b + 48), qpack = Read<uint32_t>(b + 52);
    const bool direct = Read<uint32_t>(b + 28) == 1;
    const uint32_t quant = Read<uint32_t>(b + 56);
    const uint32_t mask = direct ? Lsl(1, qbits) - 1 : ~Lsl(UINT32_MAX, qpack);
    for (int i = 0; i < dimension; ++i) {
        const uint32_t value = payload & mask;
        payload = Lsr(payload, direct ? qbits : qpack);
        if (direct) output[i] = value;
        else {
            const uint32_t bytes = static_cast<int32_t>(qbits) > 8 ? 2 : 1;
            const uint64_t p = uint64_t(quant) + uint64_t(value) * bytes;
            if (!readable(p, bytes)) return false;
            output[i] = bytes == 1 ? m[p] : Read<uint16_t>(m + p);
        }
    }
    const uint32_t mindiff = scale - Read<uint32_t>(b + 36);
    const uint32_t minimum = static_cast<int32_t>(mindiff) > 0
        ? Asr(Read<uint32_t>(b + 32), mindiff) : Lsl(Read<uint32_t>(b + 32), 0u - mindiff);
    const uint32_t deltadiff = scale - Read<uint32_t>(b + 44), delta = Read<uint32_t>(b + 40);
    for (int i = 0; i < dimension; ++i) {
        const uint32_t product = output[i] * delta;
        output[i] = minimum + (static_cast<int32_t>(deltadiff) > 0
            ? Asr(product, deltadiff) : Lsl(product, 0u - deltadiff));
    }
    // The shipped code advances its bit reader before writing the quantized
    // vector. All alias/unsupported cases were rejected before either write.
    const uint32_t total = bit + consumed, bytes = Asr(total, 3);
    Write<uint32_t>(m + pack, total & 7);
    Write<uint32_t>(m + pack + 4, data + bytes);
    Write<uint32_t>(m + pack + 8, Read<uint32_t>(m + pack + 8) - bytes);
    std::memcpy(m + out, output.data(), outbytes);
    NotifyGuestMemoryWritten(pack, 12); NotifyGuestMemoryWritten(out, outbytes);
    return true;
}

// Scalar VFP conversion/multiplication and AArch64 SIMD use the same rounding
// controls here. No unsafe fast-math/FMA and no quality-dependent arithmetic.
class FpScope {
public:
    explicit FpScope(uint32_t fpscr) {
        asm volatile("mrs %0, fpcr\n mrs %1, fpsr" : "=r"(control_), "=r"(status_));
        const uint64_t control = fpscr & 0x07C00000u;
        asm volatile("msr fpcr, %0\n msr fpsr, xzr" :: "r"(control) : "memory");
    }
    uint32_t Flags() const { uint64_t value; asm volatile("mrs %0, fpsr" : "=r"(value) :: "memory"); return uint32_t(value) & 0x9Fu; }
    ~FpScope() { asm volatile("msr fpcr, %0\n msr fpsr, %1" :: "r"(control_), "r"(status_) : "memory"); }
private:
    uint64_t control_, status_;
};

bool Floor(DynarmicHost& h, const std::array<uint32_t,16>& r,
           uint32_t cpsr, uint32_t& fpscr, uint32_t& result) {
    // This game's audio uses round-to-nearest. Preserve the original bridge
    // path for other rounding modes rather than changing its FPCR behavior.
    if ((cpsr & 0x0700FE20u) || (fpscr & 0x00F79F00u)) return false;
    const uint32_t dsp = r[0], info = r[1], memo = r[2], out = r[3], sp = r[13];
    if (!Heap(h, dsp, 84) || !Heap(h, info, 36) || sp < 512 ||
        !Heap(h, sp - 512, 512) || Overlap(sp - 512, 512, dsp, 84) ||
        Overlap(sp - 512, 512, info, 36)) return false;
    auto* m = h.GetGuestMemoryBase();
    if (!memo) {
        const uint32_t blocks = Read<uint32_t>(m + dsp + 16), index = Read<uint32_t>(m + dsp + 80);
        if (index > 1 || !Heap(h, uint64_t(blocks) + index * 4, 4) ||
            Overlap(sp - 512, 512, uint64_t(blocks) + index * 4, 4)) return false;
        const int32_t n = Read<int32_t>(m + blocks + index * 4);
        if (n < 0 || n > int32_t(kMaxFloorBins * 2)) return false;
        const size_t length = size_t(n / 2) * 4;
        if (!Heap(h, out, length) || Overlap(out, length, sp - 512, 512)) return false;
        std::memset(m + out, 0, length); NotifyGuestMemoryWritten(out, length); result = 0;
        return true;
    }
    const int32_t posts = Read<int32_t>(m + info + 28);
    if (posts <= 0 || posts > 256 || !Heap(h, memo, size_t(posts) * 4) ||
        Overlap(sp - 512, 512, memo, size_t(posts) * 4)) return false;
    if (posts <= 1) { result = 1; return true; }
    const uint32_t positions = Read<uint32_t>(m + info + 8), sorted = Read<uint32_t>(m + info + 12);
    const uint32_t multiplier = Read<uint32_t>(m + info + 32);
    if (!Heap(h, positions, size_t(posts) * 2) || !Heap(h, sorted, posts) || multiplier < 1 || multiplier > 4) return false;
    std::array<uint8_t,kMaxFloorBins> levels;
    uint32_t x = 0, y = Read<uint32_t>(m + memo) * multiplier;
    for (int post = 1; post < posts; ++post) {
        const uint32_t index = m[sorted + post];
        if (index >= uint32_t(posts)) return false;
        const uint32_t value = Read<uint32_t>(m + memo + index * 4);
        if (value != (value & 0x7FFFu)) continue;
        const uint32_t end = Read<uint16_t>(m + positions + index * 2), next = value * multiplier;
        if (end <= x || end > kMaxFloorBins || y > 255 || next > 255) return false;
        const int32_t dy = int32_t(next) - int32_t(y), dx = int32_t(end - x), base = dy / dx;
        const int32_t correction = dy < 0 ? base - 1 : base + 1;
        const int32_t remainder = std::abs(dy) - std::abs(dx * base);
        int32_t error = 0, level = int32_t(y);
        levels[x] = uint8_t(level);
        for (uint32_t bin = x + 1; bin < end; ++bin) {
            error += remainder;
            if (error >= dx) { level += correction; error -= dx; } else level += base;
            if (level < 0 || level > 255) return false;
            levels[bin] = uint8_t(level);
        }
        x = end; y = next;
    }
    const size_t length = size_t(x) * 4;
    if (!Heap(h, out, length) || (out & 3) ||
        Overlap(out, length, dsp, 84) || Overlap(out, length, info, 36) ||
        Overlap(out, length, memo, size_t(posts) * 4) ||
        Overlap(out, length, positions, size_t(posts) * 2) || Overlap(out, length, sorted, posts) ||
        Overlap(sp - 512, 512, positions, size_t(posts) * 2) || Overlap(sp - 512, 512, sorted, posts) ||
        Overlap(sp - 512, 512, out, length)) return false;
    const auto* table = m + floor_table;
    for (uint32_t bin = 0; bin < x; ++bin) {
        const uint32_t bits = Read<uint32_t>(table + levels[bin] * 4);
        // Original mutable lookup is read live. Unusual modified table values
        // retain original scalar VFP handling, including NaN payloads/FTZ.
        if (bits < 0x00800000u || bits >= 0x7F800000u) return false;
    }
    uint32_t flags;
    {
        FpScope fp(fpscr);
        uint32_t bin = 0;
        for (; bin + 4 <= x; bin += 4) {
            const int32x4_t input = vld1q_s32(reinterpret_cast<const int32_t*>(m + out + bin * 4));
            const float weights[4] = {Read<float>(table + levels[bin] * 4), Read<float>(table + levels[bin+1] * 4),
                Read<float>(table + levels[bin+2] * 4), Read<float>(table + levels[bin+3] * 4)};
            const float32x4_t scaled = vmulq_f32(vcvtq_f32_s32(input), vld1q_f32(weights));
            vst1q_f32(reinterpret_cast<float*>(m + out + bin * 4), scaled);
        }
        for (; bin < x; ++bin) {
            const int32_t input = Read<int32_t>(m + out + bin * 4);
            const float weight = Read<float>(table + levels[bin] * 4);
            float converted, scaled;
            asm volatile("scvtf %s0, %w1" : "=w"(converted) : "r"(input));
            asm volatile("fmul %s0, %s1, %s2" : "=w"(scaled) : "w"(converted), "w"(weight));
            Write<float>(m + out + bin * 4, scaled);
        }
        flags = fp.Flags();
    }
    fpscr |= flags; result = 1; NotifyGuestMemoryWritten(out, length);
    return true;
}

uint32_t Random(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

// Execute actual pinned ARM32 code with a known FPSCR and capture its return
// and FPSCR. The wrapper restores the calling JIT's original FP controls.
uint32_t Wrapper(DynarmicHost& h, uint32_t function) {
    const uint32_t code = h.GuestAlloc(64, 4);
    if (!code) return 0;
    const uint32_t words[] = {0xE92D5FF0, 0xE1A04000, 0xE1A05001, 0xEEF17A10,
        0xEEE12A10, 0xE894000F, 0xE59FC014, 0xE12FFF3C, 0xE5850000,
        0xEEF10A10, 0xE5850004, 0xEEE17A10, 0xE8BD9FF0, function};
    for (size_t i = 0; i < std::size(words); ++i) h.MemoryWrite32(code + uint32_t(i * 4), words[i]);
    h.AddExecutableRange(code, 64); h.InvalidateGuestCode(code, 64); return code;
}

void RunWrapper(DynarmicHost& h, uint32_t wrapper, uint32_t args, uint32_t result, uint32_t fpscr) {
    // A32 VMSR updates the pinned JIT state without MSR FPCR. Prepare state
    // in a completed RunFunction so the next JIT prelude loads physical FPCR.
    const uint32_t previous = h.RunFunction(prepare_fpscr, {fpscr});
    h.RunFunction(wrapper, {args, result, fpscr});
    h.RunFunction(prepare_fpscr, {previous});
}

bool Check(DynarmicHost& h, uint32_t function, bool floor, uint32_t cases, uint32_t original = 0) {
    const uint32_t block = h.GuestAlloc(kFixtureBytes, 16), wrapper = Wrapper(h, function);
    const uint32_t reference = original ? Wrapper(h, original) : wrapper;
    if (!block || !wrapper || !reference) { if (block) h.GuestFree(block); if (wrapper) h.GuestFree(wrapper); if (reference && reference != wrapper) h.GuestFree(reference); return false; }
    auto* m = h.GetGuestMemoryBase(); uint32_t random = 0xAD10CA75u;
    const uint32_t args = block + 24576, result = block + 24608, output = block + 8192;
    std::array<uint8_t,8192> expected{};
    bool passed = true;
    for (uint32_t test = 0; test < cases && passed; ++test) {
        std::memset(m + block, 0, kFixtureBytes);
        std::array<uint32_t,16> r{}; r[13] = block + 30000;
        const uint32_t initialFpscr = ((test % 4) << 22) | ((test & 4) ? 0x01000000u : 0) |
            ((test & 8) ? 0x02000000u : 0) | ((test & 16) ? 0x15u : 0);
        uint32_t fpscr = initialFpscr, returnValue = 0, outputBytes;
        if (floor) {
            const uint32_t info = block + 128, memo = block + 256, sorted = block + 1024;
            const uint32_t positions = block + 1536, blocks = block + 2048;
            const uint32_t posts = 2 + test % 31, multiplier = 1 + test % 4;
            r[0] = block; r[1] = info; r[2] = test % 17 == 0 ? 0 : memo; r[3] = output;
            Write<uint32_t>(m + block + 16, blocks); Write<uint32_t>(m + block + 80, test & 1);
            Write<uint32_t>(m + blocks, 512); Write<uint32_t>(m + blocks + 4, 1024);
            Write<uint32_t>(m + info + 8, positions); Write<uint32_t>(m + info + 12, sorted);
            Write<uint32_t>(m + info + 28, posts); Write<uint32_t>(m + info + 32, multiplier);
            for (uint32_t i = 0; i < posts; ++i) {
                m[sorted + i] = uint8_t(i); Write<uint16_t>(m + positions + i * 2, uint16_t(i * 17));
                uint32_t value = Random(random) % (256 / multiplier);
                if (i > 0 && i + 1 < posts && test % 5 == 0) value |= 0x8000;
                Write<uint32_t>(m + memo + i * 4, value);
            }
            outputBytes = 8192;
            for (uint32_t i = 0; i < outputBytes; i += 4) Write<uint32_t>(m + output + i, Random(random));
        } else {
            const uint32_t pack = block + 64, bits = block + 128, tree = block + 512, quant = block + 4096;
            r[0] = block; r[1] = pack; r[2] = output; r[3] = Random(random) % 40;
            const uint32_t width = test % 3 == 0 ? 1 : test % 3 == 1 ? 2 : 4;
            const uint32_t packed = width == 4 || (test & 4) ? 1 : 2;
            const uint32_t leaf = width == 1 ? 0x80u : width == 2 ? 0x8000u : 0x80000000u;
            const uint32_t dimension = 1 + test % 64;
            Write<uint32_t>(m + block, dimension); Write<uint32_t>(m + block + 12, test % 23 == 0 ? 0 : test % 7 == 0 ? 32 : 8);
            Write<uint32_t>(m + block + 16, tree); Write<uint32_t>(m + block + 20, width);
            Write<uint32_t>(m + block + 24, packed); Write<uint32_t>(m + block + 28, test & 1 ? 1 : 2);
            Write<uint32_t>(m + block + 32, Random(random)); Write<uint32_t>(m + block + 36, test % 40);
            Write<uint32_t>(m + block + 40, Random(random)); Write<uint32_t>(m + block + 44, (test * 3) % 40);
            Write<uint32_t>(m + block + 48, test % 6 == 0 ? 32 : 4 + test % 12);
            Write<uint32_t>(m + block + 52, 4); Write<uint32_t>(m + block + 56, quant);
            Write<uint32_t>(m + pack, test % 8); Write<uint32_t>(m + pack + 4, bits);
            Write<uint32_t>(m + pack + 8, (test & 16) ? UINT32_MAX : 32);
            for (uint32_t i = 0; i < 8; ++i) m[bits + i] = uint8_t(Random(random));
            const auto setNode = [&](uint32_t i, uint32_t v) {
                if (width == 1) m[tree + i] = uint8_t(v);
                else if (width == 2) Write<uint16_t>(m + tree + i * 2, uint16_t(v));
                else Write<uint32_t>(m + tree + i * 4, v);
            };
            if (packed == 1) {
                setNode(0, 1); setNode(1, 2);
                if (test % 9 == 0) {
                    setNode(0, leaf | (Random(random) & (leaf - 1)));
                    setNode(1, leaf | (Random(random) & (leaf - 1)));
                } else if (test % 11 == 0) {
                    for (uint32_t i = 2; i < 6; ++i) setNode(i, i + 1);
                    for (uint32_t i = 6; i < 14; ++i) setNode(i, leaf | (Random(random) & (leaf - 1)));
                } else for (uint32_t i = 2; i < 6; ++i) setNode(i, leaf | (Random(random) & (leaf - 1)));
            } else {
                // Split leaves read a neighboring low word and the previous
                // node's sign. Cover immediate, two-level and four-level trees.
                if (test % 9 == 0) {
                    setNode(0, leaf | 5); setNode(1, leaf | 6); setNode(2, 7); setNode(3, 8);
                } else if (test % 11 == 0) {
                    setNode(0, 4); setNode(1, 4); setNode(4, 8); setNode(5, 8);
                    setNode(8, 12); setNode(9, 12); setNode(12, leaf | 5);
                    setNode(13, leaf | 6); setNode(14, 7); setNode(15, 8);
                } else {
                    setNode(0, 4); setNode(1, 4); setNode(4, leaf | 5); setNode(5, leaf | 6); setNode(6, 7);
                }
            }
            for (uint32_t i = 0; i < 64; i += 2) Write<uint16_t>(m + quant + i, uint16_t(Random(random)));
            outputBytes = dimension * 4;
        }
        std::array<uint8_t,8192> initialOutput{}; std::array<uint8_t,12> initialPack{};
        std::memcpy(initialOutput.data(), m + output, outputBytes);
        std::memcpy(initialPack.data(), m + block + 64, 12);
        const bool native = floor ? Floor(h, r, 0x10, fpscr, returnValue) : Decode(h, r, 0x10);
        for (uint32_t i = 0; i < 4; ++i) Write<uint32_t>(m + args + i * 4, r[i]);
        std::array<uint8_t,12> expectedPack{};
        if (!native) {
            passed = floor && (initialFpscr & 0x00C00000u) && fpscr == initialFpscr && returnValue == 0 &&
                std::memcmp(initialOutput.data(), m + output, outputBytes) == 0 &&
                std::memcmp(initialPack.data(), m + block + 64, 12) == 0;
            if (!passed) break;
            NotifyGuestMemoryWritten(block, kFixtureBytes);
            RunWrapper(h, reference, args, result, initialFpscr);
            fpscr = Read<uint32_t>(m + result + 4); returnValue = Read<uint32_t>(m + result);
        }
        std::memcpy(expected.data(), m + output, outputBytes);
        std::memcpy(expectedPack.data(), m + block + 64, 12);
        std::memcpy(m + output, initialOutput.data(), outputBytes); std::memcpy(m + block + 64, initialPack.data(), 12);
        NotifyGuestMemoryWritten(block, kFixtureBytes);
        RunWrapper(h, wrapper, args, result, initialFpscr);
        passed = std::memcmp(expected.data(), m + output, outputBytes) == 0 &&
            std::memcmp(expectedPack.data(), m + block + 64, 12) == 0 &&
            Read<uint32_t>(m + result + 4) == fpscr && (!floor || Read<uint32_t>(m + result) == returnValue);
        if (!passed) {
            char property[PROP_VALUE_MAX]{};
            if (__system_property_get("debug.bl2.perf_tests", property) == 1 && property[0] == '1') {
                uint32_t offset = 0;
                while (offset < outputBytes && Read<uint32_t>(expected.data() + offset) == Read<uint32_t>(m + output + offset)) offset += 4;
                __android_log_print(ANDROID_LOG_ERROR, "BL2PerfCheck",
                    "audio-%s test%u mismatch offset=%u native=%08x guest=%08x input=%08x return=%08x/%08x pack=%d fpscr=%08x/%08x",
                    floor ? "floor" : "codebook", test, offset,
                    offset < outputBytes ? Read<uint32_t>(expected.data() + offset) : 0,
                    offset < outputBytes ? Read<uint32_t>(m + output + offset) : 0,
                    offset < outputBytes ? Read<uint32_t>(initialOutput.data() + offset) : 0,
                    returnValue, Read<uint32_t>(m + result), std::memcmp(expectedPack.data(), m + block + 64, 12),
                    Read<uint32_t>(m + result + 4), fpscr);
            }
        }
    }
    if (reference != wrapper) { h.InvalidateGuestCode(reference, 64); h.GuestFree(reference); }
    h.InvalidateGuestCode(wrapper, 64); h.GuestFree(wrapper); h.GuestFree(block); return passed;
}

bool CheckGuards(DynarmicHost& h, bool floor) {
    const uint32_t block = h.GuestAlloc(8192, 16); if (!block) return false;
    auto* m = h.GetGuestMemoryBase(); std::array<uint8_t,8192> before{}; bool passed = true;
    for (uint32_t test = 0; test < 6 && passed; ++test) {
        std::memset(m + block, 0, 8192);
        std::array<uint32_t,16> r{}; r[0] = block; r[13] = block + 8000;
        uint32_t fpscr = 0x15, result = 0x12345678, cpsr = test == 0 ? 0x30 : test == 1 ? 0x210 : 0x10;
        if (floor) {
            r[1] = block + 128; r[2] = block + 256; r[3] = block + 2048;
            Write<uint32_t>(m + r[1] + 8, block + 512); Write<uint32_t>(m + r[1] + 12, block + 1024);
            Write<uint32_t>(m + r[1] + 28, 2); Write<uint32_t>(m + r[1] + 32, 1);
            Write<uint32_t>(m + r[2], 64); Write<uint32_t>(m + r[2] + 4, 128);
            Write<uint16_t>(m + block + 514, 8); m[block + 1025] = 1;
            if (test == 2) fpscr |= 0x100;
            if (test == 3) r[3] = r[2];
            if (test == 4) Write<uint32_t>(m + r[2] + 4, 400);
            if (test == 5) Write<uint16_t>(m + block + 514, 0);
        } else {
            r[1] = block + 64; r[2] = block + 2048;
            Write<uint32_t>(m + block, 4); Write<uint32_t>(m + block + 12, 8);
            Write<uint32_t>(m + block + 16, block + 512); Write<uint32_t>(m + block + 20, 4);
            Write<uint32_t>(m + block + 24, 1); Write<uint32_t>(m + block + 28, 1);
            Write<uint32_t>(m + block + 40, 1); Write<uint32_t>(m + block + 48, 4);
            Write<uint32_t>(m + r[1] + 4, block + 128); Write<uint32_t>(m + r[1] + 8, 16);
            Write<uint32_t>(m + block + 512, 0x80000001);
            if (test == 2) r[2] = block;
            if (test == 3) Write<uint32_t>(m + r[1], 8);
            if (test == 4) Write<uint32_t>(m + block, 257);
            if (test == 5) Write<uint32_t>(m + block + 16, r[2]);
        }
        const uint32_t originalFpscr = fpscr;
        std::memcpy(before.data(), m + block, 8192);
        const bool accepted = floor ? Floor(h, r, cpsr, fpscr, result) : Decode(h, r, cpsr);
        passed = !accepted && std::memcmp(before.data(), m + block, 8192) == 0 &&
            fpscr == originalFpscr && result == 0x12345678;
    }
    h.GuestFree(block); return passed;
}

bool Witness(DynarmicHost& h, uint32_t address, uint32_t bytes, const char* expected) {
    if (!h.GetElfLoader().IsCodeRange(address, bytes)) return false;
    ObbSha256 hash; hash.Update(h.GetGuestMemoryBase() + address, bytes);
    return std::strcmp(ObbSha256::Hex(hash.Finalize()).data(), expected) == 0;
}
uint32_t Fallback(DynarmicHost& h, uint32_t function) {
    const uint32_t veneer = h.GuestAlloc(24, 4); if (!veneer) return 0;
    for (uint32_t offset : {0u,12u}) {
        h.MemoryWrite32(veneer + offset, h.MemoryRead32(function));
        h.MemoryWrite32(veneer + offset + 4, kLdrPc);
        h.MemoryWrite32(veneer + offset + 8, offset ? __builtin_bswap32(function + 4) : function + 4);
    }
    h.AddExecutableRange(veneer, 24); h.InvalidateGuestCode(veneer, 24); return veneer;
}
void Return(Dynarmic::A32::Jit& jit) {
    const uint32_t lr = jit.Regs()[14];
    jit.SetCpsr((jit.Cpsr() & ~0x20u) | ((lr & 1u) << 5));
    jit.Regs()[15] = lr & ~1u;
}
void Report(const char* name, bool passed) {
    char property[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.bl2.perf_tests", property) == 1 && property[0] == '1')
        __android_log_print(ANDROID_LOG_INFO, "BL2PerfCheck",
            "audio-%s %s patched64 fallback16 atomic-reject6: %s", name,
            std::strcmp(name, "floor") == 0 ? "reference512 nativeRN128 unsupportedRM384" : "original/native512",
            passed ? "PASS" : "FAIL");
}
}

void SetBl2AudioDecodeMask(uint32_t mask) { enabled.store(mask & installed, std::memory_order_relaxed); }

bool InstallBl2AudioDecodeFastpaths(DynarmicHost& h, uint32_t startup_mask) {
    char property[PROP_VALUE_MAX]{};
    uint32_t selected = startup_mask;
    if (__system_property_get("debug.bl2.audio_decode", property) > 0) {
        char* end = nullptr; const auto value = std::strtoul(property, &end, 10);
        if (!end || *end || value > 3) return false;
        selected = uint32_t(value);
    }
    if (selected > 3) return false;
    if (!selected) return true;
    if (h.IsJitRunning() || installed) return false;
    const uint32_t base = h.GetElfLoader().GetBaseAddress();
    const uint32_t decode = h.FindSymbol(kDecodeSymbol), floor = h.FindSymbol(kFloorSymbol);
    if (h.GetElfLoader().GetImageSize() != 0x03089000 ||
        ((selected & 1) && (decode != base + kDecodeRva ||
            !Witness(h, decode, 1012, "0F368A899B41F4BAE3FC0CFE9A2B98CFC21FA7431AB5B0EDAFC5CF8109683890") ||
            !Witness(h, base + 0x026DA358, 132, "E01D9CED883CF6D599E6A67AC15C05A3C8E5803E9058D12F48754E6BBA750E31") ||
            !Witness(h, base + 0x026DA3DC, 44, "C240C2343E30C3014D670025598F341C0C92C867BA5B6D0183B6D84968DDD2DE"))) ||
        ((selected & 2) && (floor != base + kFloorRva ||
            !Witness(h, floor, 460, "293C0C26DF9D047D072FE0D53A304B7279595E31870A494089FED38B2A8E58F4")))) return false;
    floor_table = h.FindSymbol("fFLOOR_fromdB_LOOKUP");
    if ((selected & 2) && (floor_table != base + kFloorTableRva ||
        uint64_t(floor_table) + 1024 > h.GetGuestMemorySize() ||
        h.MemoryRead32(base + 0x02F395B8) != floor_table)) return false;
    prepare_fpscr = h.GetThunkManager().RegisterThunk("bl2_audio_check_fpscr",
        [](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            const uint32_t previous = jit.Fpscr(); jit.SetFpscr(jit.Regs()[0]); jit.Regs()[0] = previous;
        });
    if (!prepare_fpscr) return false;
    bool passed = true;
    if (selected & 1) { passed = CheckGuards(h, false) && Check(h, decode, false, 512); if (!passed) { Report("codebook", false); return false; } }
    if (selected & 2) { passed = CheckGuards(h, true) && Check(h, floor, true, 512); if (!passed) { Report("floor", false); return false; } }
    const uint32_t decodeFallback = selected & 1 ? Fallback(h, decode) : 0;
    const uint32_t floorFallback = selected & 2 ? Fallback(h, floor) : 0;
    if (((selected & 1) && !decodeFallback) || ((selected & 2) && !floorFallback)) return false;
    uint32_t decodeThunk = 0, floorThunk = 0;
    if (selected & 1) decodeThunk = h.GetThunkManager().RegisterThunk("bl2_audio_codebook_fastpath",
        [decode,decodeFallback](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            if (jit.Regs()[15] != decode + 4) { jit.HaltExecution(Dynarmic::HaltReason::UserDefined3); return; }
            if (!(enabled.load(std::memory_order_relaxed) & 1) || !Decode(host, jit.Regs(), jit.Cpsr()))
                jit.Regs()[15] = decodeFallback + ((jit.Cpsr() & 0x200u) ? 12u : 0u);
            else Return(jit);
        });
    if (selected & 2) floorThunk = h.GetThunkManager().RegisterThunk("bl2_audio_floor_fastpath",
        [floor,floorFallback](Dynarmic::A32::Jit& jit, DynarmicHost& host) {
            if (jit.Regs()[15] != floor + 4) { jit.HaltExecution(Dynarmic::HaltReason::UserDefined3); return; }
            uint32_t fpscr = jit.Fpscr(), result = 0;
            if (!(enabled.load(std::memory_order_relaxed) & 2) || !Floor(host, jit.Regs(), jit.Cpsr(), fpscr, result))
                jit.Regs()[15] = floorFallback + ((jit.Cpsr() & 0x200u) ? 12u : 0u);
            else { jit.Regs()[0] = result; jit.SetFpscr(fpscr); Return(jit); }
        });
    if (((selected & 1) && !decodeThunk) || ((selected & 2) && !floorThunk)) return false;
    const std::array<uint32_t,2> addresses{decode,floor};
    const std::array<uint32_t,2> originals{0xE92D47F0u,0xE92D4FF0u};
    const std::array<uint32_t,2> thunks{decodeThunk,floorThunk};
    std::array<uint32_t,2> instructions{};
    for (uint32_t i = 0; i < 2; ++i) if (selected & (1u << i)) {
        instructions[i] = h.MemoryRead32(thunks[i]);
        if ((instructions[i] & 0xFF000000u) != 0xEF000000u || !(instructions[i] & 0x00FFFFFFu)) return false;
    }
    for (uint32_t i = 0; i < 2; ++i) if (selected & (1u << i)) {
        // A single SVC reaches the handler for both CPSR data endiannesses.
        // LDR PC + a little-endian literal cannot reach a BE fallback safely.
        h.MemoryWrite32(addresses[i], instructions[i]); h.InvalidateGuestCode(addresses[i], 4);
    }
    installed = selected; enabled.store(selected, std::memory_order_relaxed);
    for (uint32_t i = 0; i < 2 && passed; ++i) if (selected & (1u << i)) {
        passed = Check(h, addresses[i], i == 1, 64, i ? floorFallback : decodeFallback);
        enabled.store(0, std::memory_order_relaxed);
        if (passed) passed = Check(h, addresses[i], i == 1, 16, i ? floorFallback : decodeFallback);
        enabled.store(selected, std::memory_order_relaxed);
        Report(i ? "floor" : "codebook", passed);
    }
    if (!passed) {
        enabled.store(0, std::memory_order_relaxed); installed = 0;
        for (uint32_t i = 0; i < 2; ++i) if (selected & (1u << i)) {
            h.MemoryWrite32(addresses[i], originals[i]); h.InvalidateGuestCode(addresses[i], 4);
        }
        return false;
    }
    return true;
}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Dynarmic::A32 { class Jit; }

// Diagnostic only: immutable, private to one idle-created PhysX primary core.
// A non-null all-NULL table selects Dynarmic's existing precise-PC fallback.
// It also preserves MemoryWrite callbacks when software dirty tracking is on.
using Bl2PhysxPageTable = std::array<std::uint8_t*, 1u << 20>;

struct Bl2PhysxCookieWatch {
    enum class Phase { Idle, InitialStore, Initialized, Tripped };
    enum class Cause { None, WrongInitialStore, Writer, AlreadyCorrupt, ObservedAtExit, Reentry };
    enum class Source { GuestStore, NativeCallback, NativeInterval, Observation };
    struct Event {
        Cause cause = Cause::None;
        Source source = Source::Observation;
        std::uint32_t address = 0, bytes = 0, before = 0, after = 0;
        std::uint64_t value = 0;
        std::uint32_t pc = 0, lr = 0;
    } event;
    struct NativeToken {
        Bl2PhysxCookieWatch* watch = nullptr;
        std::uint64_t sequence = 0;
        std::uint32_t slot = 0, before = 0, address = 0, pc = 0, lr = 0;
        std::size_t bytes = 0;
    };

    Dynarmic::A32::Jit* owner = nullptr;
    std::size_t context_slot = 0;
    Phase phase = Phase::Idle;
    std::uint32_t stack_low = 0, stack_top = 0, slot = 0, expected = 0;
    std::uint32_t entry_sp = 0, guard_pointer = 0, scene = 0;
    std::uint64_t sequence = 0;
    unsigned overlaps = 0, initial_stores = 0, successful_exits = 0;
    bool reported = false;

    bool Arm(std::uint32_t address, std::uint32_t guard) {
        if (phase != Phase::Idle || std::uint64_t{address} + 4 > (std::uint64_t{1} << 32))
            return false;
        slot = address;
        expected = guard;
        ++sequence;
        overlaps = initial_stores = 0;
        reported = false;
        event = {};
        phase = Phase::InitialStore;
        return true;
    }
    bool Overlaps(std::uint32_t address, std::size_t bytes) const {
        // Subtraction avoids overflow even for a malformed native length.
        return phase != Phase::Idle && bytes && address < std::uint64_t{slot} + 4 &&
            (address >= slot || bytes > std::size_t(slot - address));
    }
    bool Trip(Cause cause, Source source, std::uint32_t address, std::uint32_t bytes,
              std::uint64_t value, std::uint32_t before, std::uint32_t after) {
        if (phase == Phase::Tripped) return false;
        event = {cause, source, address, bytes, before, after, value, 0, 0};
        phase = Phase::Tripped;
        return true;
    }
    bool Observe(std::uint32_t address, unsigned bytes, std::uint64_t value, std::uint32_t before) {
        if (phase == Phase::Tripped || bytes > 8 || !Overlaps(address, bytes)) return false;
        ++overlaps;
        std::uint32_t after = before;
        for (unsigned i = 0; i != 4; ++i) {
            const std::uint64_t byte_address = std::uint64_t{slot} + i;
            if (byte_address >= address && byte_address < std::uint64_t{address} + bytes) {
                const unsigned source_shift = unsigned(byte_address - address) * 8;
                const unsigned target_shift = i * 8;
                after = (after & ~(0xFFu << target_shift)) |
                    (std::uint32_t((value >> source_shift) & 0xFFu) << target_shift);
            }
        }
        if (phase == Phase::InitialStore && address == slot && bytes == 4 && after == expected) {
            ++initial_stores;
            phase = Phase::Initialized;
            return false;
        }
        if (phase == Phase::Initialized && before == expected && after == expected) return false;
        return Trip(phase == Phase::InitialStore ? Cause::WrongInitialStore :
                    before != expected ? Cause::AlreadyCorrupt : Cause::Writer,
                    Source::GuestStore, address, bytes, value, before, after);
    }
    bool Disarm(std::uint32_t current) {
        if (phase != Phase::Initialized) return false;
        if (current != expected) {
            Trip(Cause::ObservedAtExit, Source::Observation, slot, 4, current, current, current);
            return false;
        }
        ++successful_exits;
        phase = Phase::Idle;
        return true;
    }
};

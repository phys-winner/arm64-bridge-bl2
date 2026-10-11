#include "bl2_movie_skip.h"

#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include "dynarmic_host.h"
#include "elf_loader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace {

// FFullScreenMovieBink of guest A0B06EF5 (GFullScreenMovie). Every field below is
// used by the instructions InstallBl2MovieSkip compares word for word:
//   +0x3c  float countdown: Tick subtracts the frame time while it is non-zero
//          and calls StopMovie(0) at <= 0; StopCurrentAndPlayNext resets it.
//   +0x50  open Bink handle: Tick pumps the movie only while it is set.
//   +0x84/+0x8c  startup movie count and index, -1 outside the startup sequence.
//          There StopMovie(0) first sets +0x90 (skip the last startup movie) and
//          the next StopMovie(0) stops the current one; ProcessNextStartupSequence
//          then ends the sequence instead of playing a marked last movie.
// The player's own skip flag (+0x5c) is not used: the original keeps the NVIDIA
// logo unskippable, and the countdown stops any movie.
constexpr std::uint32_t kGlobal = 0x03066764;           // GFullScreenMovie
constexpr std::uint32_t kVtable = 0x02D38C08;           // _ZTV20FFullScreenMovieBink
constexpr std::uint32_t kObjectBytes = 0x100;
constexpr std::uint32_t kStopCountdown = 0x3c, kBink = 0x50, kStartupIndex = 0x8c;

constexpr std::uint32_t kStopMovie[] = {  // FFullScreenMovieBink::StopMovie(unsigned int)
    0xE3A02000, 0xE3510000, 0xE92D4008, 0xE1A03000, 0xE58020F0, 0xE58020E8, 0x1A00000C,
    0xE5900090, 0xE1500002, 0x1A000009, 0xE593208C, 0xE3720001, 0x0A000006, 0xE593C084,
    0xE24CC001, 0xE152000C, 0xAA000002, 0xE3A02001, 0xE5832090, 0xE8BD8008, 0xE2711001,
    0xE1A00003, 0x33A01000, 0xEBFFF7E4, 0xE3A00001, 0xE8BD8008};
constexpr std::uint32_t kTickCountdown[] = {  // FFullScreenMovieBink::Tick(float)+0x10
    0xEDD07A0F, 0xE24DD038, 0xEEF57A40, 0xEEF1FA10, 0x0A000004, 0xEE777AC8, 0xEEF57AC0,
    0xEDC07A0F, 0xEEF1FA10, 0x9A000038};
constexpr std::uint32_t kTickPump[] = {  // Tick+0x4C: PumpMovie only with a Bink handle
    0xE5943050, 0xE3530000, 0x0A000003, 0xE1A00004, 0xEBFFFDE0, 0xE3500000};
constexpr std::uint32_t kTickStop[] = {0xE3A01000, 0xEBFFD2FB};  // Tick+0x11C: StopMovie(0)
constexpr std::uint32_t kStopCurrent[] = {  // StopCurrentAndPlayNext(unsigned int) entry
    0xE92D4FF0, 0xE1A05000, 0xE5900050, 0xE3A02000, 0xE3E03000, 0xE59F9300, 0xE3A0C000,
    0xE24DD01C, 0xE1A0B001, 0xE58520F0, 0xE58520E8, 0xE1500002, 0xE585C03C, 0xE08F9009,
    0xE5853044, 0xE5853040};
constexpr std::uint32_t kNextLast[] = {  // ProcessNextStartupSequence+0xF4: next == count-1
    0xE3500000, 0xBAFFFFED, 0xE2444001, 0xE0603180, 0xE0502004, 0xE2724000, 0xE1A03103,
    0xE0B44002, 0xE59D2018, 0xE58D4020};
constexpr std::uint32_t kNextSkipCheck[] = {  // +0x14C: a marked last movie is not played
    0xE59D2020, 0xE3520000, 0x0A000002, 0xE59B3090, 0xE3530000, 0x1A000065};
constexpr std::uint32_t kNextSkipEnd[] = {  // +0x2FC: clear the mark, index -1
    0xE3A03000, 0xE3E02000, 0xE1A00003, 0xE58B3090, 0xE58B208C, 0xEAFFFF46};

struct CodeCheck { std::uint32_t rva; const std::uint32_t* words; std::size_t count; };
constexpr CodeCheck kChecks[] = {
    {0x010F4650, kStopMovie, std::size(kStopMovie)},
    {0x010FF94C, kTickCountdown, std::size(kTickCountdown)},
    {0x010FF988, kTickPump, std::size(kTickPump)},
    {0x010FFA58, kTickStop, std::size(kTickStop)},
    {0x010F2644, kStopCurrent, std::size(kStopCurrent)},
    {0x010F3444, kNextLast, std::size(kNextLast)},
    {0x010F349C, kNextSkipCheck, std::size(kNextSkipCheck)},
    {0x010F364C, kNextSkipEnd, std::size(kNextSkipEnd)},
};

DynarmicHost* s_host = nullptr;
std::uint32_t s_base = 0;

// The current player if it is the Bink one (a fallback player has no movies).
std::uint32_t MoviePlayer() {
    const std::uint32_t movie = s_host->MemoryRead32(s_base + kGlobal);
    if (movie < 0x10000 || std::uint64_t{movie} + kObjectBytes > s_host->GetGuestMemorySize())
        return 0;
    return s_host->MemoryRead32(movie) == s_base + kVtable + 8 ? movie : 0;
}

std::uint32_t StateOf(std::uint32_t movie) {
    if (!movie) return 0;
    std::uint32_t state = 0;
    if (s_host->MemoryRead32(movie + kBink)) state |= bl2::touch::kMoviePlaying;
    if (s_host->MemoryRead32(movie + kStartupIndex) != 0xFFFFFFFFu) state |= bl2::touch::kMovieStartup;
    return state;
}

std::uint32_t State() { return StateOf(MoviePlayer()); }

// Any thread: the player's own Tick stops the movie at its next frame (in the
// startup sequence at the second one, see above).
bool Skip() {
    const std::uint32_t movie = MoviePlayer();
    if (!(StateOf(movie) & bl2::touch::kMoviePlaying)) return false;
    constexpr float kNextFrame = 1e-4f;  // below any frame time
    std::uint32_t bits = 0;
    std::memcpy(&bits, &kNextFrame, sizeof(bits));
    s_host->MemoryWrite32(movie + kStopCountdown, bits);
    return true;
}

}  // namespace

bool InstallBl2MovieSkip(DynarmicHost& host) {
    const std::uint32_t base = host.GetElfLoader().GetBaseAddress();
    if (host.FindSymbol("GFullScreenMovie") != base + kGlobal ||
        host.FindSymbol("_ZTV20FFullScreenMovieBink") != base + kVtable) return false;
    for (const auto& check : kChecks)
        for (std::size_t i = 0; i < check.count; ++i)
            if (host.MemoryRead32(base + check.rva + static_cast<std::uint32_t>(4 * i)) != check.words[i])
                return false;
    s_host = &host;
    s_base = base;
    bl2::touch::setMovieAccess({State, Skip});
    return true;
}

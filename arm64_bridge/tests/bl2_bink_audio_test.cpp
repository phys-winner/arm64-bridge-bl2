#include "bl2_bink_audio.h"
#include "bl2_locale_host_port.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    assert(argc == 6 || argc == 8);
    const int audio = open(argv[1], O_RDONLY), main = open(argv[2], O_RDONLY);
    const int original = open(argv[5], O_RDONLY);
    assert(audio >= 0 && main >= 0 && original >= 0);
    struct stat st{};
    assert(fstat(audio, &st) == 0 && st.st_size > 0);
    Bl2BinkAudio movie;
    const uint64_t base = std::strtoull(argv[3], nullptr, 10);
    const uint64_t size = std::strtoull(argv[4], nullptr, 10);
    const uint64_t audio_base = argc == 8 ? std::strtoull(argv[6], nullptr, 10) : 0;
    const uint64_t audio_bytes = argc == 8 ? std::strtoull(argv[7], nullptr, 10) : st.st_size;
    assert(movie.Load(audio, audio_base, audio_bytes, main, base, size));
    Bl2BinkAudio truncated;
    assert(!truncated.Load(audio, audio_base, 79, main, base, size));
    assert(!truncated.Load(audio, audio_base, audio_bytes - 1, main, base, size));
    const int retained_audio = dup(audio);
    assert(retained_audio >= 0);
    movie.SetAudioFd(retained_audio);
    close(audio);
    std::array<uint8_t, 65536> actual{}, expected{};
    uint64_t offset = 0;
    while (offset < movie.Size()) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(actual.size(), movie.Size() - offset));
        assert(movie.Read(actual.data(), count, offset) == count);
        assert(pread(original, expected.data(), count, offset) == count);
        assert(std::memcmp(actual.data(), expected.data(), count) == 0);
        offset += count;
    }
    for (uint64_t at = 0; at < movie.Size(); at += 7919) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(97, movie.Size() - at));
        assert(movie.Read(actual.data(), count, at) == count);
        assert(pread(original, expected.data(), count, at) == count);
        assert(std::memcmp(actual.data(), expected.data(), count) == 0);
    }
    assert(movie.Read(actual.data(), actual.size(), movie.Size()) == 0);
    assert(movie.Read(actual.data(), 1, -1) == -1);
    assert(movie.Read(nullptr, 1, 0) == -1);
    std::printf("PASS: %s, %llu reconstructed bytes, full stream/random reads/EOF/bounds\n",
                argv[1], static_cast<unsigned long long>(movie.Size()));
    close(retained_audio); close(main); close(original);
}

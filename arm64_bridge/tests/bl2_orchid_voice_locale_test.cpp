#define BL2_LOCALE_HOST_TEST 1
#include "../src/bl2_locale.cpp"
#include <cassert>
#include <cstdio>

int main(int argc, char** argv) {
    assert(argc == 6);
    const int base = open(argv[1], O_RDONLY | O_CLOEXEC);
    assert(base >= 0);
    s_entries[0].apk_fd = base;s_entries[0].file_size = 5676447;
    s_entries[1].apk_fd = base;s_component_valid[0] = true;
    const auto before = s_entries[0];
    Zip good, bad;
    assert(good.Open(argv[2]) && bad.Open(argv[3]));
    s_russian = false;
    assert(!IndexOrchidRussianText(good) && s_orchid_voice_fd < 0);
    s_russian = true;
    assert(!IndexOrchidRussianText(bad));
    assert(s_composed_locale_fd < 0 && s_orchid_voice_fd < 0 &&
        std::memcmp(&before, &s_entries[0], sizeof(before)) == 0);
    assert(IndexOrchidRussianText(good));
    assert(s_orchid_voice_fd >= 0 && good.fd == -1 && s_entries[1].apk_fd == base);
    s_active.store(true);
    // An English entry remains intact; only the explicit Orchid VO alias wins.
    ExternalEntry english;english.path = kOrchidVoicePaths[0];english.entry.apk_fd = base;
    s_external_entries.push_back(english);
    for (size_t i = 0; i < 2; ++i) {
        const auto* entry = FindBl2LocaleEntry(kOrchidVoicePaths[i]);
        assert(entry == &s_orchid_voice_entries[i] && entry->apk_fd == s_orchid_voice_fd);
        const int source = open(argv[4 + i], O_RDONLY | O_CLOEXEC);assert(source >= 0);
        struct stat status{};assert(fstat(source, &status) == 0 && status.st_size == entry->file_size);
        for (uint64_t offset : {uint64_t(0), uint64_t(1709), uint64_t(entry->file_size - 97)}) {
            std::array<uint8_t, 97> actual{}, expected{};
            assert(ReadExact(source, status.st_size, offset, expected.data(), expected.size()));
            assert(ReadExact(entry->apk_fd, entry->data_offset + entry->file_size,
                entry->data_offset + offset, actual.data(), actual.size()));
            assert(actual == expected);
        }
        close(source);
    }
    assert(s_external_entries[0].entry.apk_fd == base);
    assert(!FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/Audio_Banks_Orchid.pck"));
    assert(!FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/English(US)/Audio_Banks_Anemone.pck"));
    assert(!FindBl2LocaleEntry("../WillowGame/CookedAndroid_DXT/English(US)/../Audio_Banks_Orchid.pck"));
    assert(FileForPath("WillowGame/CookedAndroid_DXT/English(US)/Audio_Banks.pck") < kFileCount);
    assert(!IndexOrchidRussianText(bad));
    close(base);close(s_composed_locale_fd);close(s_orchid_voice_fd);
    std::puts("PASS: RU-only pinned three-file role, valid-index wrong-bank atomic rejection, retained archive descriptor, two exact aliases, English entry/core isolation and bounded PCK range reads");
}

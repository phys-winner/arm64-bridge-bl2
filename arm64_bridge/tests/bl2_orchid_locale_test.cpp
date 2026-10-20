#define BL2_LOCALE_HOST_TEST 1
#include "../src/bl2_locale.cpp"

#include <cassert>
#include <cstdio>

int main(int argc, char** argv) {
    assert(argc == 5);
    const int base = open(argv[1], O_RDONLY | O_CLOEXEC);
    assert(base >= 0);
    s_entries[0].apk_fd = base;
    s_entries[0].data_offset = 0;
    s_entries[0].file_size = 5676447;
    s_entries[1].apk_fd = base;
    s_component_valid[0] = true;
    const Bl2LocaleEntry before = s_entries[0];
    s_russian = false;
    SetBl2ExternalObbDirectory(argv[2]);
    IndexExternalArchives();
    assert(s_composed_locale_fd < 0 && s_entries[0].apk_fd == base);
    s_russian = true;
    SetBl2ExternalObbDirectory(argv[3]);
    IndexExternalArchives();
    assert(s_composed_locale_fd < 0 && std::memcmp(&before, &s_entries[0], sizeof(before)) == 0);
    SetBl2ExternalObbDirectory(argv[2]);
    IndexExternalArchives();
    assert(s_composed_locale_fd >= 0 && s_entries[0].apk_fd == s_composed_locale_fd);
    assert(s_entries[0].file_size == 6447212 && s_entries[0].data_offset == 0);
    assert(s_entries[1].apk_fd == base);  // Fonts retain their base descriptor.
    std::vector<uint8_t> actual(6447212), expected(6447212);
    const int reference = open(argv[4], O_RDONLY | O_CLOEXEC);
    assert(reference >= 0);
    assert(ReadExact(reference, expected.size(), 0, expected.data(), expected.size()));
    assert(ReadExact(s_composed_locale_fd, actual.size(), 0, actual.data(), actual.size()));
    assert(actual == expected);
    const int first = dup(s_composed_locale_fd), second = dup(s_composed_locale_fd);
    assert(first >= 0 && second >= 0);
    uint8_t a[41]{}, b[29]{};
    assert(ReadExact(first, actual.size(), 1709, a, sizeof(a)));
    assert(ReadExact(second, actual.size(), 3077, b, sizeof(b)));
    assert(std::memcmp(a, expected.data() + 1709, sizeof(a)) == 0);
    assert(std::memcmp(b, expected.data() + 3077, sizeof(b)) == 0);
    assert(!ReadExact(first, actual.size(), actual.size() - 1, a, sizeof(a)));
    const Bl2LocaleEntry published = s_entries[0];
    Zip repeated;
    assert(repeated.Open((std::filesystem::path(argv[2]) /
        "dlc_captain_scarlett_russian_voice.31.com.tt2kgames.Borderlands2.obb").string().c_str()));
    assert(!IndexOrchidRussianText(repeated));
    assert(std::memcmp(&published, &s_entries[0], sizeof(published)) == 0);
    assert(s_external_entries.empty());
    close(first); close(second); close(reference); close(s_composed_locale_fd); close(base);
    std::puts("PASS: RU-only role, valid-index wrong-delta rejection without mutation, exact composed descriptor, font preservation, bounded range reads and immutable publication");
}

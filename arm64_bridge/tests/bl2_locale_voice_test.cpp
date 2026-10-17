#define BL2_LOCALE_HOST_TEST 1
#include "../src/bl2_locale.cpp"

#include <cassert>
#include <cstdio>

int main(int argc, char** argv) {
    assert(argc == 6 || argc == 7);
    const int main_fd = open(argv[5], O_RDONLY);
    struct stat st{};
    assert(main_fd >= 0 && fstat(main_fd, &st) == 0);
    SetBl2LocaleMainObb(main_fd, 0, st.st_size);
    if (argc == 7) {
        assert(!IndexBl2CacheBase(argv[6]));
        assert(!HasBl2RelocatedCache() && s_component_valid[0] && s_payload_valid);
        s_active.store(true);
        assert(FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/Coalesced_INT.bin"));
        assert(!FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/Textures.tfc"));
        assert(!FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/CharTextures.tfc"));
        assert(!FindBl2LocaleEntry("WillowGame/Movies/Attract.bik"));
        s_active.store(false);
    }
    for (int i = 1; i <= 2; ++i) {
        Zip zip;
        assert(zip.Open(argv[i]));
        assert(IndexLocaleComponent(zip, 0));
        assert(IndexLocaleComponent(zip, 1));
        for (size_t f = 0; f < kLocaleFileCount; ++f) {
            const auto* entry = zip.Find(s_entries[f].name);
            assert(entry && s_entries[f].file_size ==
                (s_entries[f].bink ? s_entries[f].bink->Size() : entry->bytes));
        }
    }
    Zip compressed;
    assert(compressed.Open(argv[3]));
    assert(!IndexLocaleComponent(compressed, 1));
    Zip truncated;
    assert(!truncated.Open(argv[4]));
    std::puts("PASS: text/voice ignore descriptor/payload hashes, CRC and pinned sizes; compressed/truncated ranges rejected");
    close(main_fd);
}

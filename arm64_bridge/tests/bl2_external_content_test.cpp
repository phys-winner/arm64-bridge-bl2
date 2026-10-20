#define BL2_LOCALE_HOST_TEST 1
#include "../src/bl2_locale.cpp"
#include "obb_split_inventory.h"

#include <cassert>
#include <cstdio>

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::filesystem::path fixtures(argv[1]);
    for (const auto* name : {"good.obb", "bad-hash.obb", "bad-size.obb", "bad-path.obb", "duplicate.obb", "unindexed.obb", "bad-local-crc.obb"}) {
        Zip zip;
        zip.trusted_installed = false;
        std::vector<const ZipEntry*> files;
        const bool accepted = zip.Open((fixtures / name).string().c_str()) &&
            VerifyExternalIndex(zip, "WillowGame/CookedAndroid_DXT/", files);
        assert(accepted == (std::string_view(name) == "good.obb"));
    }
    SetBl2ExternalObbDirectory(fixtures.string().c_str());
    s_russian = false;
    s_active.store(true);
    IndexExternalArchives();
    const auto* entry = FindBl2LocaleEntry("../../WillowGame/CookedAndroid_DXT/Orchid_Startup_SF.xxx");
    assert(entry && entry->file_size == 560789 && entry->apk_fd >= 0);
    const auto* raid = FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/Orchid_Caves_Raid_P.xxx");
    assert(raid && raid->file_size == 434595);
    assert(ExternalPackageForRequest("Orchid_Startup_SF"));
    assert(ExternalPackageForRequest("Orchid_Startup_SF.XxX"));
    assert(ExternalPackageForRequest("../../WillowGame/CookedAndroid_DXT/orchid_startup_sf.xxx"));
    assert(!ExternalPackageForRequest("WillowGame"));
    assert(!ExternalPackageForRequest("Orchid_Startup"));
    assert(!FindBl2LocaleEntry("../../WillowGame/../../savegame/0/Player1"));
    assert(!FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/Fixture.xxx"));
    auto absent = OpenExternalObbPair(fixtures.string().c_str());
    assert(absent.status == ObbSplitScanStatus::NoObbEntry);
    {
        FILE* file = std::fopen((fixtures / kMainObbName).string().c_str(), "wb");
        assert(file); std::fputs("not an OBB", file); std::fclose(file);
    }
    auto broken = OpenExternalObbPair(fixtures.string().c_str());
    assert(broken.status == ObbSplitScanStatus::Rejected && broken.fds[0] < 0 && broken.fds[1] < 0);
    auto pair = OpenExternalObbPair(argv[2]);
    assert(pair.status == ObbSplitScanStatus::Verified);
    SetBl2LocaleMainObb(pair.fds[0], 0, pair.sizes[0]);
    Zip voice;
    voice.trusted_installed = false;
    std::vector<const ZipEntry*> files;
    assert(voice.Open((std::filesystem::path(argv[2]) / "russian_voice.31.com.tt2kgames.Borderlands2.obb").string().c_str()));
    assert(VerifyExternalIndex(voice, "assets/bl2_locale/ru_voice/", files));
    assert(IndexLocaleComponent(voice, 1));
    const auto* banks = FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/English(US)/Audio_Banks.pck");
    const auto* movie = FindBl2LocaleEntry("WillowGame/Movies/MegaIntro.bik");
    assert(banks && banks->file_size > 0 && movie && movie->bink);
    for (const int fd : pair.fds) close(fd);
    std::puts("PASS: external index hash/CRC/path/duplicate/bounds rejection, version-31 DLC mapping, original main/patch names and Russian banks/movie binding");
}

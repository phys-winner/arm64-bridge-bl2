#define BL2_LOCALE_HOST_TEST 1
#include "../src/bl2_locale.cpp"
#include <cassert>
#include <cstdio>

int main(int argc, char** argv) {
    assert(argc == 4);
    // The original five-file candidate is read through the actual ZIP parser,
    // CRC/SHA verifier and pinned-index selector, without mounting a directory.
    Zip wrong_role;
    wrong_role.trusted_installed = false;
    assert(!wrong_role.Open(argv[1]));
    Zip reordered;
    reordered.trusted_installed=false; reordered.orchid_metadata=true;
    std::vector<const ZipEntry*> reordered_files;
    assert(reordered.Open(argv[2]) && VerifyExternalIndex(reordered,"DLC/Orchid/",reordered_files));
    RememberOrchidArchive("dlc_captain_scarlett_registration.31.com.tt2kgames.Borderlands2.obb",reordered,reordered_files);
    assert(s_orchid_archive_rows[static_cast<size_t>(OrchidRole::Metadata)].empty());
    Zip wrong_digest;
    wrong_digest.trusted_installed=false; wrong_digest.orchid_metadata=true;
    std::vector<const ZipEntry*> wrong_files;
    assert(wrong_digest.Open(argv[3]) && !VerifyExternalIndex(wrong_digest,"DLC/Orchid/",wrong_files));
    Zip metadata;
    metadata.trusted_installed = false;
    metadata.orchid_metadata = true;
    std::vector<const ZipEntry*> files;
    assert(metadata.Open(argv[1]) && VerifyExternalIndex(metadata, "DLC/Orchid/", files) && files.size() == 5);
    RememberOrchidArchive("untrusted_registration.obb", metadata, files);
    assert(s_orchid_archive_rows[static_cast<size_t>(OrchidRole::Metadata)].empty());
    RememberOrchidArchive("dlc_captain_scarlett_registration.31.com.tt2kgames.Borderlands2.obb", metadata, files);
    assert(s_orchid_archive_rows[static_cast<size_t>(OrchidRole::Metadata)].size() == 5);
    std::vector<bl2_orchid::FileRow> output{{"sentinel",-1,0,0}};
    assert(!CollectOrchidRows(output) && output.size() == 1 && output[0].path == "sentinel");
    assert(!s_orchid_admitted.load() && !Bl2OrchidOwnsApp(207870));
    assert(!Bl2OrchidChildren("DLC") && !FindBl2LocaleEntry("DLC/Orchid/Lic/contver.bin"));

    // Isolated row-assembly fixture, deliberately not an archive-admission
    // fixture. This never toggles the production frozen-closure data gate.
    FILE* source = nullptr;
    assert(tmpfile_s(&source) == 0 && source);
    constexpr unsigned char original[] = {0xFE,0x2B,0x03,0,3,0,0,0,'X'};
    assert(std::fwrite(original, 1, sizeof(original), source) == sizeof(original));
    assert(std::fflush(source) == 0);
    const int fd = _fileno(source);
    for (auto& rows : s_orchid_archive_rows) rows.clear();
    for (const auto& alias : bl2_orchid_admission::kAliases) {
        auto& rows = s_orchid_archive_rows[static_cast<size_t>(alias.role)];
        if (std::none_of(rows.begin(), rows.end(), [&](const auto& row) { return row.path == alias.source; }))
            rows.push_back({alias.source,fd,0,std::string_view(alias.path).ends_with("steam_0.bin") ? 8u : 1u});
    }
    s_orchid_voice_fd = fd;
    s_composed_locale_fd = fd;  // Synthetic composed-text prerequisite only.
    s_russian = true;
    for (auto& file : s_orchid_voice_entries) { file.apk_fd=fd; file.data_offset=8; file.file_size=1; }
    assert(CollectOrchidRows(output));
    assert(output.size() == std::size(bl2_orchid_admission::kAliases) + 2);
    const auto good_rows = s_orchid_archive_rows;
    for (size_t role=0; role<kOrchidRoleCount; ++role) {
        s_orchid_archive_rows = good_rows;
        s_orchid_archive_rows[role].clear();
        const auto before=output.size();
        assert(!CollectOrchidRows(output) && output.size()==before);
    }
    s_orchid_archive_rows = good_rows;
    s_orchid_archive_rows[static_cast<size_t>(OrchidRole::Metadata)].back().offset=1;
    assert(!CollectOrchidRows(output));
    s_orchid_archive_rows = good_rows;
    s_russian=false; assert(!CollectOrchidRows(output)); s_russian=true;
    s_orchid_voice_fd=-1; assert(!CollectOrchidRows(output)); s_orchid_voice_fd=fd;
    s_composed_locale_fd=-1; assert(!CollectOrchidRows(output)); s_composed_locale_fd=fd;
    assert(!Bl2OrchidOwnsApp(207870));

    std::string decoded="sentinel";
    const auto decode = [&](std::string text) { text+='\0'; return bl2_orchid::DecodeDlcRoot(
        reinterpret_cast<const uint8_t*>(text.data()),text.size(),decoded); };
    assert(decode("/observed/game/") && decoded=="/observed/game/DLC");
    assert(decode("/different/runtime/game") && decoded=="/different/runtime/game/DLC");
    for (const auto& bad : {std::string{},std::string("/"),std::string("relative/game/"),std::string("//game/"),
        std::string("/game/../else/"),std::string("/game/./"),std::string("/game\\evil/"),std::string("/game:evil/"),
        std::string("/game/*/"),std::string("/game/?/"),std::string("/game/\n/"),std::string("/")+std::string(256,'a'),
        std::string("/game/")+std::string(250,'a')+"/"+std::string(250,'b')+"/suffix"}) {
        decoded="sentinel"; assert(!decode(bad) && decoded=="sentinel");
    }
    const uint8_t embedded[]={'/','a',0,'b',0};
    assert(!bl2_orchid::DecodeDlcRoot(embedded,sizeof(embedded),decoded));
    const uint8_t no_nul[]={'/','a','b'};
    assert(!bl2_orchid::DecodeDlcRoot(no_nul,sizeof(no_nul),decoded));
    assert(!bl2_orchid::DecodeDlcRoot(nullptr,3,decoded));
    assert(PublishBl2OrchidMount(output,"/different/runtime/game/DLC"));
    // Publication alone is not entitlement; both predicates are required.
    assert(!Bl2OrchidOwnsApp(207870));
    s_orchid_admitted.store(true);
    assert(Bl2OrchidOwnsApp(207870));
    for(uint32_t other : {0u,1u,3u,207869u,207871u,0xffffffffu}) assert(!Bl2OrchidOwnsApp(other));
    assert(std::fclose(source)==0);
    std::puts("ORCHID_ADMISSION_MECHANICS_PASS: actual metadata CRC/SHA/index/namespace; closed default; atomic row assembly; exact 8B license; original-root decoding; published+admitted AppId-only ownership");
}

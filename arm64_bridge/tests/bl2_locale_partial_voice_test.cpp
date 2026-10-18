// Voice splits whose movies cannot be bound keep their dialogue banks.
// Build for any POSIX/Android target with zlib, e.g. NDK clang:
//   --target=aarch64-linux-android24 -std=c++20 -I arm64_bridge/src
//   arm64_bridge/tests/bl2_locale_partial_voice_test.cpp arm64_bridge/src/bl2_bink_audio.cpp -lz
// and run it with a writable scratch directory as the only argument.
#define BL2_LOCALE_HOST_TEST 1
#include "../src/bl2_locale.cpp"

#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace {
void Put16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value)); out.push_back(uint8_t(value >> 8));
}
void Put32(std::vector<uint8_t>& out, uint32_t value) {
    Put16(out, value & 0xFFFF); Put16(out, value >> 16);
}
// A minimal STORED archive: local headers, central directory, end record.
std::string WriteZip(const std::string& path, std::initializer_list<std::pair<std::string, std::string>> files) {
    std::vector<uint8_t> data, directory;
    for (const auto& [name, body] : files) {
        const uint32_t offset = uint32_t(data.size());
        const uint32_t crc = uint32_t(crc32(0, reinterpret_cast<const Bytef*>(body.data()), uInt(body.size())));
        const auto header = [&](std::vector<uint8_t>& out, bool central) {
            Put32(out, central ? 0x02014B50 : 0x04034B50);
            if (central) Put16(out, 20);
            Put16(out, 20); Put16(out, 0); Put16(out, 0); Put16(out, 0); Put16(out, 0);
            Put32(out, crc); Put32(out, uint32_t(body.size())); Put32(out, uint32_t(body.size()));
            Put16(out, uint16_t(name.size())); Put16(out, 0);
            if (central) { Put16(out, 0); Put16(out, 0); Put16(out, 0); Put32(out, 0); Put32(out, offset); }
            out.insert(out.end(), name.begin(), name.end());
        };
        header(data, false);
        data.insert(data.end(), body.begin(), body.end());
        header(directory, true);
    }
    const uint32_t start = uint32_t(data.size());
    data.insert(data.end(), directory.begin(), directory.end());
    Put32(data, 0x06054B50); Put16(data, 0); Put16(data, 0);
    Put16(data, uint16_t(files.size())); Put16(data, uint16_t(files.size()));
    Put32(data, uint32_t(directory.size())); Put32(data, start); Put16(data, 0);
    FILE* file = std::fopen(path.c_str(), "wb");
    assert(file && std::fwrite(data.data(), 1, data.size(), file) == data.size());
    std::fclose(file);
    return path;
}
const std::string kVoice = "assets/bl2_locale/ru_voice/";
const std::string kMovies[] = {"MegaIntro", "TC_Marcus", "TC_Scooter", "TC_Tannis", "TC_Zed"};
void Reset() {
    s_entries = {}; s_component_valid = {}; s_payload_valid = false;
    for (auto& fd : s_apk_fds) if (fd >= 0) close(std::exchange(fd, -1));
}
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string dir = argv[1];
    s_active.store(true);
    // R3 audio-only movies without the main OBB video (OBBs outside the splits
    // and none in Android/obb here): the banks stay, the movies keep English.
    {
        const auto path = WriteZip(dir + "/r3.apk", {
            {kVoice + "pack.json", "{}"},
            {kVoice + "payload/English(US)/Audio_Banks.pck", "BANKS"},
            {kVoice + "payload/English(US)/Audio_Streaming.pck", "STREAM"},
            {kVoice + "payload/Movies/MegaIntro.bka", "BL2BAUD1 no video"},
            {kVoice + "payload/Movies/TC_Marcus.bka", "BL2BAUD1 no video"},
            {kVoice + "payload/Movies/TC_Scooter.bka", "BL2BAUD1 no video"},
            {kVoice + "payload/Movies/TC_Tannis.bka", "BL2BAUD1 no video"},
            {kVoice + "payload/Movies/TC_Zed.bka", "BL2BAUD1 no video"}});
        Zip zip;
        assert(s_main_fd < 0 && zip.Open(path.c_str()) && IndexLocaleComponent(zip, 1));
        const auto* bank = FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/English(US)/Audio_Banks.pck");
        assert(bank && bank->file_size == 5 && !bank->bink);
        assert(FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/English(US)/Audio_Streaming.pck"));
        for (const auto& movie : kMovies) assert(!FindBl2LocaleEntry(("WillowGame/Movies/" + movie + ".bik").c_str()));
        Reset();
    }
    // Voice splits before R3 carried each movie whole: it plays as it is.
    {
        const auto path = WriteZip(dir + "/old.apk", {
            {kVoice + "pack.json", "{}"},
            {kVoice + "payload/English(US)/Audio_Banks.pck", "BANKS"},
            {kVoice + "payload/English(US)/Audio_Streaming.pck", "STREAM"},
            {kVoice + "payload/Movies/MegaIntro.bik", "BIKi whole movie"},
            {kVoice + "payload/Movies/TC_Marcus.bik", "BIKi whole movie"},
            {kVoice + "payload/Movies/TC_Scooter.bik", "BIKi whole movie"},
            {kVoice + "payload/Movies/TC_Tannis.bik", "BIKi whole movie"},
            {kVoice + "payload/Movies/TC_Zed.bik", "BIKi whole movie"}});
        Zip zip;
        assert(zip.Open(path.c_str()) && IndexLocaleComponent(zip, 1));
        for (const auto& movie : kMovies) {
            const auto* entry = FindBl2LocaleEntry(("WillowGame/Movies/" + movie + ".bik").c_str());
            assert(entry && !entry->bink && entry->file_size == 16 &&
                   std::string_view(entry->name).ends_with(movie + ".bik"));
        }
        Reset();
    }
    // Without the dialogue banks the component is still refused.
    {
        const auto path = WriteZip(dir + "/movies-only.apk", {
            {kVoice + "pack.json", "{}"},
            {kVoice + "payload/Movies/MegaIntro.bik", "BIKi whole movie"}});
        Zip zip;
        assert(zip.Open(path.c_str()) && !IndexLocaleComponent(zip, 1) && !s_component_valid[1]);
        assert(!FindBl2LocaleEntry("WillowGame/CookedAndroid_DXT/English(US)/Audio_Banks.pck"));
    }
    s_active.store(false);
    std::puts("PASS: voice banks survive unbound movies; whole .bik movies of older voice splits play; banks stay required");
}

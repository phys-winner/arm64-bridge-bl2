#define BL2_LOCALE_HOST_TEST 1
#include "../src/bl2_locale.cpp"
#include <cassert>
#include <cstdio>
#include <thread>

int main() {
    constexpr char root[] = "/observed/game/DLC";
    constexpr char file[] = "DLC/Orchid/Compat/Content/Fixture.xxx";
    struct stat status{};
    int access_result = 123;
    assert(!FindBl2LocaleEntry(file));
    assert(Bl2OrchidKind("DLC") == bl2_orchid::NodeKind::Missing);
    assert(!Bl2OrchidChildren("DLC") && !Bl2OrchidStat("DLC", status));
    assert(!Bl2OrchidAccess(file, 0, access_result) && access_result == 123);
    assert(!PublishBl2OrchidMount({}, root));
    std::FILE* source = nullptr;
    assert(tmpfile_s(&source) == 0 && source);
    constexpr char payload[] = "HEAD:DATA:TAIL";
    assert(std::fwrite(payload, 1, sizeof(payload) - 1, source) == sizeof(payload) - 1);
    assert(std::fflush(source) == 0);
    const int fd = _fileno(source);
    std::vector<bl2_orchid::FileRow> rows{
        {file, fd, 5, 4},
        {"DLC/Orchid/Compat/contver.bin", fd, 0, 5},
        {"DLC/Orchid/Lic/contver.bin", fd, 0, 5},
        {"DLC/Orchid/Lic/Config/ConfigInstall/WillowDlc.ini", fd, 0, 5},
        {"DLC/Orchid/Lic/Audio/Empty.bin", fd, 5, 0},
    };
    const auto reject = [&](const std::vector<bl2_orchid::FileRow>& bad, const char* absolute) {
        assert(!PublishBl2OrchidMount(bad, absolute));
        assert(!s_orchid_published.load() && s_orchid_mount_fds.empty());
        assert(!Bl2OrchidChildren("DLC") && !FindBl2LocaleEntry(file));
    };
    reject(rows, nullptr); reject(rows, "DLC"); reject(rows, "/observed/../DLC");
    auto duplicate = rows; duplicate.push_back({"dlc/orchid/compat/content/FIXTURE.XXX", fd, 5, 4}); reject(duplicate, root);
    reject({{"DLC/Orchid/a", fd, 0, 1}, {"DLC/Orchid/a/b", fd, 0, 1}}, root);
    reject({{"../../DLC/Orchid/a", fd, 0, 1}}, root);
    reject({{"DLC/Orchid/../a", fd, 0, 1}}, root);
    reject({{"DLC/Other/a", fd, 0, 1}}, root);
    reject({{"DLC/Orchid/a", -1, 0, 1}}, root);
    reject({{"DLC/Orchid/a", fd, sizeof(payload), 1}}, root);
    reject({{"DLC/Orchid/" + std::string(128, 'a'), fd, 0, 1}}, root);
    const auto base_before = s_entries;
    assert(PublishBl2OrchidMount(rows, root));
    assert(!s_active.load());  // Independent of language/flat probe activation.
    assert(s_orchid_mount_fds.size() == 1);
    assert(std::fclose(source) == 0);  // Caller/temporary archive can now close.
    auto* entry = FindBl2LocaleEntry(file);
    assert(entry && entry->apk_fd >= 0 && entry->apk_fd != fd && entry->data_offset == 5 && entry->file_size == 4);
    assert(FindBl2LocaleEntry("..\\..\\dlc\\ORCHID\\compat\\Content\\Fixture.XXX") == entry);
    assert(FindBl2LocaleEntry("/observed/game/DLC/Orchid/Compat/Content/Fixture.xxx") == entry);
    assert(!FindBl2LocaleEntry("/wrong/DLC/Orchid/Compat/Content/Fixture.xxx"));
    assert(!FindBl2LocaleEntry("DLC/Orchid/Compat/Content/Fixture.xxx/"));
    assert(std::memcmp(base_before.data(), s_entries.data(), sizeof(s_entries)) == 0);
    std::array<uint8_t, 4> actual{};
    assert(ReadExact(entry->apk_fd, entry->data_offset + entry->file_size, entry->data_offset, actual.data(), actual.size()));
    assert(std::memcmp(actual.data(), "DATA", 4) == 0);
    std::array<uint8_t, 5> too_big{};
    assert(!ReadExact(entry->apk_fd, entry->data_offset + entry->file_size, entry->data_offset, too_big.data(), too_big.size()));
    assert(Bl2OrchidStat(file, status) && (status.st_mode & S_IFREG) && status.st_size == 4 && (status.st_mode & 0777) == 0444);
    assert(Bl2OrchidStat("../../DLC/Orchid/Compat/", status) && (status.st_mode & S_IFDIR) && (status.st_mode & 0777) == 0555);
    for (int mode : {0, 4}) assert(Bl2OrchidAccess(file, mode, access_result) && access_result == 0);
    for (int mode : {1, 2, 6}) assert(Bl2OrchidAccess(file, mode, access_result) && access_result == -1 && errno == EACCES);
    assert(Bl2OrchidAccess(file, 8, access_result) && access_result == -1 && errno == EINVAL);
    for (int mode : {0, 1, 4, 5}) assert(Bl2OrchidAccess("DLC/Orchid", mode, access_result) && access_result == 0);
    assert(Bl2OrchidAccess("DLC/Orchid", 2, access_result) && access_result == -1 && errno == EACCES);
    const auto* children = Bl2OrchidChildren("DLC/Orchid/Compat");
    assert(children && children->size() == 2 && (*children)[0].name == "Content" && (*children)[1].name == "contver.bin");
    for (const auto& child : *children) {
        const std::string path = "DLC/Orchid/Compat/" + child.name;
        assert(Bl2OrchidKind(path.c_str()) == child.kind && Bl2OrchidStat(path.c_str(), status));
    }
    assert(!Bl2OrchidChildren(file) && !Bl2OrchidStat("DLC/Other", status));
    auto* empty = FindBl2LocaleEntry("DLC/Orchid/Lic/Audio/Empty.bin");
    assert(empty && empty->file_size == 0 && Bl2OrchidStat(empty->name, status) && status.st_size == 0);
    assert(!PublishBl2OrchidMount(rows, root));
    assert(FindBl2LocaleEntry(file) == entry && s_orchid_mount_fds.size() == 1);
    std::thread a([&] { for (int i = 0; i < 1000; ++i) assert(FindBl2LocaleEntry(file) == entry); });
    std::thread b([&] { for (int i = 0; i < 1000; ++i) assert(Bl2OrchidChildren("../../DLC/Orchid/Compat") == children); });
    a.join(); b.join();
    std::puts("ORCHID_MOUNT_INTEGRATION_PASS: empty old behavior, atomic publish, FD lifetime/ranges, stable aliases, directory/stat/access agreement");
}

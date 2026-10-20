#define BL2_LOCALE_HOST_TEST 1
#include "../src/bl2_locale.cpp"
#include <cassert>
#include <cstdio>

int main(int argc,char** argv) {
    assert(argc==3);
    const int base=open(argv[1],O_RDONLY|O_CLOEXEC);
    assert(base>=0);
    struct stat original{};assert(fstat(base,&original)==0 && original.st_size==5676447);
    s_entries[0].apk_fd=base;s_entries[0].file_size=original.st_size;
    s_component_valid[0]=true;s_russian=true;s_obb_directory=argv[2];
    IndexExternalArchives();
    for(const auto& rows:s_orchid_archive_rows)assert(!rows.empty());
    assert(s_orchid_voice_fd>=0 && s_composed_locale_fd>=0);
    std::vector<bl2_orchid::FileRow> rows;
    assert(CollectOrchidRows(rows) && rows.size()==150);
    assert(s_orchid_admitted.load()==bl2_orchid_admission::kFaithfulClosureReady);
    assert(s_orchid_pending_rows.size()==150);
    assert(!Bl2OrchidChildren("DLC") && !Bl2OrchidOwnsApp(207870));
    // Host-only publication fixture: actual Android root reading is separately
    // bounded/compiled, and its runtime value must come from the original ELF.
    assert(PublishBl2OrchidMount(rows,"/observed/host/game/DLC"));
    assert(Bl2OrchidOwnsApp(207870)==bl2_orchid_admission::kFaithfulClosureReady);
    const auto* root=Bl2OrchidChildren("/observed/host/game/DLC");
    assert(root && root->size()==1 && (*root)[0].name=="Orchid");
    const auto* license=FindBl2LocaleEntry("../../DLC/Orchid/Lic/Licenses/steam_0.bin");
    assert(license && license->file_size==8);
    uint8_t expected[]={0xfe,0x2b,3,0,3,0,0,0},actual[8]{};
    assert(ReadExact(license->apk_fd,license->data_offset+license->file_size,license->data_offset,actual,8));
    assert(std::memcmp(actual,expected,8)==0);
    for(int fd:s_external_fds)assert(close(fd)==0);
    assert(close(s_orchid_voice_fd)==0);assert(close(base)==0);
    // Published duplicates survive temporary archive/source descriptor closure.
    for(const auto& row:rows) {
        const auto* entry=FindBl2LocaleEntry(row.path.c_str());
        assert(entry && entry->file_size==row.size);
        struct stat status{};assert(Bl2OrchidStat(row.path.c_str(),status) && status.st_size==row.size);
        int result=123;assert(Bl2OrchidAccess(row.path.c_str(),4,result) && result==0);
        uint8_t first=0;
        assert(row.size && ReadExact(entry->apk_fd,entry->data_offset+entry->file_size,entry->data_offset,&first,1));
        assert(!ReadExact(entry->apk_fd,entry->data_offset+entry->file_size,entry->data_offset+entry->file_size,&first,1));
    }
    std::printf("ORCHID_ACTUAL_ARCHIVE_PIPELINE_PASS: roles=4 source_packages=138 aliases=150 RU_text_composer=exact per_file_CRC_SHA=all retained_FD_reads=150 admission_gate=%u original_root_hardware=not_executed\n",
        unsigned(bl2_orchid_admission::kFaithfulClosureReady));
}

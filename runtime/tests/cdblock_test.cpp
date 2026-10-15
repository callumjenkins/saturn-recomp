#include "saturn.h"
#include "test.h"

// Mednafen's drive, worked by hand: 256 000 clocks BUSY, 12 sectors' time, then 28 clocks a sector back
// or 26 forward, and a sector's time more going back or 150 or more forward, at 44 100 x 256 Hz. Then
// a sector's time reading the target's subcode before it plays.
TEST(a_seek_back_costs_what_mednafens_drive_charges) {
    uint64_t busy = 0;
    CHECK_EQ(cd_seek_ns(27214, 10725, 150, &busy),
             (256000ull + 903168 + 16489 * 28 + 75264 + 75264) * 1000000000ull / 11289600);
    CHECK_EQ(busy, 256000ull * 1000000000ull / 11289600);
}

TEST(a_short_seek_forward_has_no_extra_sector) {
    CHECK_EQ(cd_seek_ns(1000, 1100, 150, nullptr), (256000ull + 903168 + 100 * 26 + 75264) * 1000000000ull / 11289600);
    CHECK_EQ(cd_seek_ns(1000, 1150, 150, nullptr),
             (256000ull + 903168 + 150 * 26 + 75264 + 75264) * 1000000000ull / 11289600);
}

TEST(a_seek_to_audio_reads_its_subcode_at_audio_speed) {
    CHECK_EQ(cd_seek_ns(1000, 1100, 75, nullptr), (256000ull + 903168 + 100 * 26 + 150528) * 1000000000ull / 11289600);
}

TEST(the_accurate_drive_reads_at_double_speed) {
    g_cfg.accurate_drive = true;
    CHECK_EQ(cd_data_rate(), 150);
    g_cfg.accurate_drive = false;
    CHECK_EQ(cd_data_rate(), 300);
}

// A sector's time at double speed is 1/150 s.
TEST(the_first_directory_read_finds_the_volume_descriptor_before_it_seeks_to_the_directory) {
    g_cfg.accurate_drive = true;
    uint64_t busy = 0, first_busy = 0;
    const uint64_t sector = 1000000000ull / 150;
    CHECK_EQ(cd_fs_read_ns(150, 1000, 2, true, &busy),
             cd_seek_ns(150, 166, 150, &first_busy) + sector + cd_seek_ns(167, 1000, 150, nullptr) + 2 * sector);
    CHECK_EQ(busy, first_busy);
    CHECK_EQ(cd_fs_read_ns(150, 1000, 2, false, nullptr), cd_seek_ns(150, 1000, 150, nullptr) + 2 * sector);
    g_cfg.accurate_drive = false;
}

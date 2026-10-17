#include "saturn.h"
#include "state.h"
#include "test.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

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

namespace {

uint16_t ask(uint16_t cr1, uint16_t cr2 = 0, uint16_t cr3 = 0, uint16_t cr4 = 0) {
    cd_write(0x90018, cr1, 2), cd_write(0x9001C, cr2, 2), cd_write(0x90020, cr3, 2), cd_write(0x90024, cr4, 2);
    uint16_t out = (uint16_t)cd_read(0x90018, 2);
    cd_read(0x90024, 2);
    return out;
}

uint16_t cd_rom_bit() { return ask(0x0000) & 0x80; }

// A MODE1/2048 disc of zeros, FAD 150 to 409, opened, and the CD block reset on the host's clock.
void data_disc() {
    char tmpl[] = "/tmp/cdblock_test_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::vector<uint8_t> img(260 * 2048);
    FILE* f = std::fopen((dir + "/d.bin").c_str(), "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    f = std::fopen((dir + "/d.cue").c_str(), "w");
    std::fprintf(f, "FILE \"d.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n");
    std::fclose(f);
    CHECK_EQ(cdrom_open(dir + "/d.cue"), true);
    g_cfg.realtime = true;
    cd_init();
}

// Ticks the drive every millisecond until `done`, for at most `ms`; whether it came.
template <class F> bool tick_until(F done, int ms) {
    for (int i = 0; i < ms; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cd_tick();
        if (done()) return true;
    }
    return false;
}

void play(uint32_t fad, uint32_t sectors) {
    ask((uint16_t)(0x1080 | fad >> 16), (uint16_t)fad, (uint16_t)(0x0080 | sectors >> 16), (uint16_t)sectors);
}

}  // namespace

TEST(a_play_answers_with_the_cd_rom_bit_then_clears_it_until_it_reads_data) {
    data_disc();
    g_cfg.accurate_drive = true;
    CHECK_EQ(cd_rom_bit(), 0x80);
    CHECK_EQ(ask(0x1080, 150, 0x0080, 20) & 0x80, 0x80);       // Play Disc, FAD 150 for 20 sectors
    CHECK_EQ(tick_until([] { return cd_rom_bit() == 0; }, 5), true);
    CHECK_EQ(tick_until([] { return cd_rom_bit() != 0; }, 2000), true);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

TEST(a_play_into_a_full_buffer_still_sets_the_cd_rom_bit) {
    data_disc();
    play(150, 250);
    CHECK_EQ(tick_until([] { return (cd_read(0x90008, 2) & 0x0008) != 0; }, 3000), true);   // BFUL
    play(150, 20);
    CHECK_EQ(tick_until([] { return cd_rom_bit() == 0; }, 5), true);
    CHECK_EQ(tick_until([] { return cd_rom_bit() != 0; }, 500), true);
    g_cfg.realtime = false;
}

TEST(a_seek_to_stop_sets_the_cd_rom_bit) {
    data_disc();
    g_cfg.accurate_drive = true;
    play(150, 20);
    CHECK_EQ(tick_until([] { return cd_rom_bit() == 0; }, 5), true);
    ask(0x1100);                                                // Seek Disc to 0: stop
    CHECK_EQ(cd_rom_bit(), 0x80);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

TEST(a_dump_keeps_the_cd_rom_bit) {
    data_disc();
    g_cfg.accurate_drive = true;
    play(150, 20);
    CHECK_EQ(tick_until([] { return cd_rom_bit() == 0; }, 5), true);
    State out;
    cdpos_state(out);
    cd_init();
    CHECK_EQ(cd_rom_bit(), 0x80);
    State in;
    in.loading = true;
    in.data = out.data;
    cdpos_state(in);
    CHECK_EQ(cd_rom_bit(), 0);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

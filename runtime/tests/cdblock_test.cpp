#include "saturn.h"
#include "state.h"
#include "test.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

// The checks come 17 712 of Mednafen's clocks, 1 568 877 ns, after each read, a sector apart.
TEST(a_pause_shows_at_the_drive_s_first_status_check_after_now) {
    const uint64_t t0 = 1000000000, check = 1568877, sector = 1000000000ull / 150;
    CHECK_EQ(cd_pause_ns(t0 + 1000, t0, 150), t0 + check);
    CHECK_EQ(cd_pause_ns(t0 + check - 1, t0, 150), t0 + check);
    CHECK_EQ(cd_pause_ns(t0 + check, t0, 150), t0 + check + sector);
    CHECK_EQ(cd_pause_ns(t0 + 3 * sector + check + 1, t0, 150), t0 + check + 4 * sector);
    CHECK_EQ(cd_pause_ns(t0 + check, t0, 75), t0 + check + 1000000000ull / 75);
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
    std::string dir = test_dir("cdblock_test");
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

// Ticks the drive until `done`, for at most `ms` of the host's time; whether it came. It ticks without sleeping:
// a play clears the CD-ROM bit for only a few milliseconds, less than a sleep can last.
template <class F> bool tick_until(F done, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
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

namespace {

uint8_t drive_status() { return ask(0x0000) >> 8 & 0x0F; }

const uint16_t kCSCT = 0x0004, kPEND = 0x0010, kEFLS = 0x0200;

// Ticks for `ms`, a bit for each drive status seen (1 << status).
unsigned statuses_seen(int ms) {
    unsigned seen = 0;
    tick_until([&] { seen |= 1u << drive_status(); return false; }, ms);
    return seen;
}

void both_endian(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> 8 * i), p[7 - i] = (uint8_t)(v >> 8 * i);
}

void record(uint8_t* p, uint32_t lba, uint32_t size, bool dir, const char* name, size_t n) {
    p[0] = (uint8_t)(33 + n + (n % 2 == 0));
    both_endian(p + 2, lba);
    both_endian(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[32] = (uint8_t)n;
    std::memcpy(p + 33, name, n);
}

// The data disc with an ISO 9660 root directory at sector 18, holding A.BIN, and then a CD-DA track 2
// of 100 sectors, opened on the accurate drive.
void iso_and_audio_disc() {
    std::string dir = test_dir("cdblock_test_iso");
    std::vector<uint8_t> img(260 * 2048);
    uint8_t* pvd = &img[16 * 2048];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    record(pvd + 156, 18, 2048, true, "\0", 1);
    img[17 * 2048] = 255;
    std::memcpy(&img[17 * 2048 + 1], "CD001", 5);
    uint8_t* root = &img[18 * 2048];
    record(root, 18, 2048, true, "\0", 1);
    record(root + 34, 18, 2048, true, "\1", 1);
    record(root + 68, 19, 2048, false, "A.BIN;1", 7);
    FILE* f = std::fopen((dir + "/d.bin").c_str(), "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    std::vector<uint8_t> audio(100 * 2352, 1);
    f = std::fopen((dir + "/a.bin").c_str(), "wb");
    std::fwrite(audio.data(), 1, audio.size(), f);
    std::fclose(f);
    f = std::fopen((dir + "/d.cue").c_str(), "w");
    std::fprintf(f, "FILE \"d.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n"
                    "FILE \"a.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n");
    std::fclose(f);
    CHECK_EQ(cdrom_open(dir + "/d.cue"), true);
    g_cfg.realtime = true;
    g_cfg.accurate_drive = true;
    cd_init();
}

}  // namespace

// Seek Disc to 0xFFFFFF just after a sector: BUSY, with no sector more, until the check a sector after the next.
TEST(a_pause_while_playing_is_busy_until_the_check_after_it_sees_the_play_s_end) {
    data_disc();
    g_cfg.accurate_drive = true;
    play(150, 250);
    cd_write(0x90008, (uint16_t)~kCSCT, 2);
    CHECK_EQ(tick_until([] { return (cd_read(0x90008, 2) & kCSCT) != 0; }, 1000), true);
    const auto asked = std::chrono::steady_clock::now();
    CHECK_EQ(ask(0x11FF, 0xFFFF) >> 8 & 0x0F, 0);
    cd_write(0x90008, (uint16_t)~kCSCT, 2);
    CHECK_EQ(tick_until([] { return drive_status() == 1; }, 100), true);
    const auto busy = std::chrono::steady_clock::now() - asked;
    CHECK_EQ(busy >= std::chrono::nanoseconds(1568877 + 1000000000 / 150 - 1000000), true);
    CHECK_EQ(busy < std::chrono::nanoseconds(1568877 + 1000000000 / 150 + 5000000), true);
    CHECK_EQ(cd_read(0x90008, 2) & kCSCT, 0);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

// PEND comes as a play ends, at a sector's read, so the next check is 1.57 ms on.
TEST(a_paused_drive_asked_to_pause_is_busy_until_its_next_check) {
    data_disc();
    g_cfg.accurate_drive = true;
    cd_write(0x90008, (uint16_t)~kPEND, 2);
    play(150, 2);
    CHECK_EQ(tick_until([] { return (cd_read(0x90008, 2) & kPEND) != 0; }, 1000), true);
    const auto asked = std::chrono::steady_clock::now();
    CHECK_EQ(ask(0x11FF, 0xFFFF) >> 8 & 0x0F, 0);
    CHECK_EQ(tick_until([] { return drive_status() == 1; }, 100), true);
    CHECK_EQ(std::chrono::steady_clock::now() - asked < std::chrono::microseconds(4000), true);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

TEST(without_the_accurate_drive_a_pause_answers_pause_at_once) {
    data_disc();
    play(150, 250);
    CHECK_EQ(ask(0x11FF, 0xFFFF) >> 8 & 0x0F, 1);
    g_cfg.realtime = false;
}

TEST(a_pause_while_playing_audio_waits_for_audio_sectors) {
    iso_and_audio_disc();
    ask(0x1000, 0x0201, 0x0000, 0x0201);                       // Play Disc, track 2
    CHECK_EQ(tick_until([] { return drive_status() == 3; }, 2000), true);
    const auto asked = std::chrono::steady_clock::now();
    CHECK_EQ(ask(0x11FF, 0xFFFF) >> 8 & 0x0F, 0);
    CHECK_EQ(tick_until([] { return drive_status() == 1; }, 200), true);
    const auto busy = std::chrono::steady_clock::now() - asked;
    CHECK_EQ(busy > std::chrono::nanoseconds(1000000000 / 75), true);
    CHECK_EQ(busy < std::chrono::nanoseconds(1568877 + 2 * 1000000000 / 75 + 5000000), true);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

TEST(a_second_pause_while_pausing_stays_busy) {
    data_disc();
    g_cfg.accurate_drive = true;
    play(150, 250);
    CHECK_EQ(tick_until([] { return drive_status() == 3; }, 1000), true);
    ask(0x11FF, 0xFFFF);
    CHECK_EQ(ask(0x11FF, 0xFFFF) >> 8 & 0x0F, 0);
    CHECK_EQ(tick_until([] { return drive_status() == 1; }, 100), true);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

// A pause during a play's seek is at once, and the seek is dropped: a pause after it never plays.
TEST(a_drive_paused_during_its_seek_stays_paused) {
    data_disc();
    g_cfg.accurate_drive = true;
    play(150, 250);
    CHECK_EQ(tick_until([] { return drive_status() == 4; }, 1000), true);
    CHECK_EQ(ask(0x11FF, 0xFFFF) >> 8 & 0x0F, 1);
    CHECK_EQ(statuses_seen(200), 1u << 1);
    cd_write(0x90008, (uint16_t)~kCSCT, 2);
    CHECK_EQ(ask(0x11FF, 0xFFFF) >> 8 & 0x0F, 0);
    CHECK_EQ(statuses_seen(60), 1u << 0 | 1u << 1);
    CHECK_EQ(cd_read(0x90008, 2) & kCSCT, 0);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

TEST(a_play_while_pausing_plays_without_pausing) {
    data_disc();
    g_cfg.accurate_drive = true;
    play(150, 250);
    CHECK_EQ(tick_until([] { return drive_status() == 3; }, 1000), true);
    ask(0x11FF, 0xFFFF);
    play(150, 250);
    unsigned seen = 0;
    CHECK_EQ(tick_until([&] { uint8_t s = drive_status(); seen |= 1u << s; return s == 3; }, 1000), true);
    CHECK_EQ(seen & 1u << 1, 0u);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

TEST(a_stop_while_pausing_stays_in_standby) {
    data_disc();
    g_cfg.accurate_drive = true;
    play(150, 250);
    CHECK_EQ(tick_until([] { return drive_status() == 3; }, 1000), true);
    ask(0x11FF, 0xFFFF);
    CHECK_EQ(ask(0x1100) >> 8 & 0x0F, 2);
    CHECK_EQ(statuses_seen(50), 1u << 2);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

TEST(a_directory_read_while_pausing_never_pauses_before_efls) {
    iso_and_audio_disc();
    play(150, 250);
    CHECK_EQ(tick_until([] { return drive_status() == 3; }, 1000), true);
    ask(0x11FF, 0xFFFF);
    cd_write(0x90008, (uint16_t)~kEFLS, 2);
    ask(0x7100);                                                // Read Directory
    bool paused = false;
    CHECK_EQ(tick_until([&] { paused |= drive_status() == 1 && !(cd_read(0x90008, 2) & kEFLS);
                              return (cd_read(0x90008, 2) & kEFLS) != 0; }, 2000), true);
    CHECK_EQ(paused, false);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

TEST(a_dump_keeps_a_pause_busy) {
    data_disc();
    g_cfg.accurate_drive = true;
    play(150, 250);
    CHECK_EQ(tick_until([] { return drive_status() == 3; }, 1000), true);
    ask(0x11FF, 0xFFFF);
    State out;
    cdblock_state(out), cdpause_state(out);
    cd_init();
    State in;
    in.loading = true;
    in.data = out.data;
    cdblock_state(in), cdpause_state(in);
    CHECK_EQ(drive_status(), 0);
    CHECK_EQ(tick_until([] { return drive_status() == 1; }, 100), true);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

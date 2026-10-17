// Borrowing a file from another disc: the directory leads to it, past this disc's lead-out.
#include "saturn.h"
#include "test.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

namespace {

void both_endian(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) { p[i] = (uint8_t)(v >> 8 * i); p[7 - i] = (uint8_t)(v >> 8 * i); }
}

void record(uint8_t* p, uint32_t lba, uint32_t size, bool dir, const char* name, size_t n) {
    p[0] = (uint8_t)(33 + n + (n % 2 == 0));
    both_endian(p + 2, lba);
    both_endian(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[32] = (uint8_t)n;
    std::memcpy(p + 33, name, n);
}

// A MODE1/2048 image whose root holds A.BIN: `data`, from sector 19. Returns the .cue's path.
std::string disc(const std::string& dir, const char* stem, const std::string& data) {
    uint32_t sectors = 19 + (uint32_t)(data.size() + 2047) / 2048;
    std::vector<uint8_t> img(sectors * 2048);
    uint8_t* pvd = &img[16 * 2048];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    record(pvd + 156, 18, 2048, true, "\0", 1);
    img[17 * 2048] = 255;
    std::memcpy(&img[17 * 2048 + 1], "CD001", 5);
    uint8_t* root = &img[18 * 2048];
    record(root, 18, 2048, true, "\0", 1);                      // .
    record(root + 34, 18, 2048, true, "\1", 1);                 // ..
    record(root + 68, 19, (uint32_t)data.size(), false, "A.BIN;1", 7);
    std::memcpy(&img[19 * 2048], data.data(), data.size());
    std::string bin = dir + "/" + stem + ".bin", cue = dir + "/" + stem + ".cue";
    FILE* f = std::fopen(bin.c_str(), "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    f = std::fopen(cue.c_str(), "w");
    std::fprintf(f, "FILE \"%s.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n", stem);
    std::fclose(f);
    return cue;
}

}  // namespace

TEST(a_borrowed_file_is_read_from_the_other_disc_past_this_ones_lead_out) {
    char tmpl[] = "/tmp/cdrom_test_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string other_data(3000, 'o');
    std::memcpy(&other_data[0], "OTHER", 5);
    CHECK_EQ(cdrom_open(disc(dir, "mine", "MINE")), true);
    std::string error;
    CHECK_EQ(cdrom_borrow(disc(dir, "theirs", other_data), "B.BIN", "", error), false);
    CHECK_EQ(cdrom_borrow(disc(dir, "theirs", other_data), "A.BIN", "", error), true);
    uint32_t fad = 0, size = 0;
    CHECK_EQ(cdrom_find("A.BIN", &fad, &size), true);
    CHECK_EQ(size, 3000u);
    CHECK_EQ(fad >= cdrom_leadout(), true);
    uint8_t raw[2352];
    CHECK_EQ(cdrom_read(fad, raw), true);
    CHECK_EQ(std::memcmp(raw + 16, "OTHER", 5), 0);
}

TEST(a_boot_copies_ip_bins_header_over_what_a_previous_program_left_in_the_bios_area) {
    char tmpl[] = "/tmp/cdrom_test_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string cue = disc(dir, "ip", "FIRST");
    uint8_t ip[0x100];
    std::memset(ip, ' ', sizeof ip);
    std::memcpy(ip, "SEGA SEGASATURN ", 16);
    const uint8_t sizes[16] = {0, 0, 0x10, 0, 0, 0, 0, 0, 0x06, 0x00, 0x20, 0x00, 0, 0, 0, 0};
    std::memcpy(ip + 0xE0, sizes, 16);
    const uint8_t first[8] = {0x06, 0x00, 0x40, 0x00, 0, 0, 0, 0};
    std::memcpy(ip + 0xF0, first, 8);
    FILE* f = std::fopen((dir + "/ip.bin").c_str(), "r+b");
    std::fwrite(ip, 1, sizeof ip, f);
    std::fclose(f);
    CHECK_EQ(cdrom_open(cue), true);
    g_cfg.save = "-";
    st8(0x06000CCC, 1);                         // a directory to open, as the NetLink's XS leaves it
    bios_boot();
    bool header = true, sizes_copied = true;
    for (uint32_t i = 0; i < 0x100; ++i) header = header && ld8(0x06000C00 + i) == ip[i];
    for (uint32_t i = 0; i < 0x20; ++i) sizes_copied = sizes_copied && ld8(0x060002A0 + i) == ip[0xE0 + i];
    CHECK_EQ(header, true);
    CHECK_EQ(sizes_copied, true);
    CHECK_EQ(ld8(0x06000CCC), (uint8_t)' ');
}

static uint16_t change_to_root() {
    cd_write(0x90008, 0, 2);                    // HIRQ cleared
    cd_write(0x90018, 0x7000, 2);
    cd_write(0x9001C, 0x0000, 2);
    cd_write(0x90020, 0x00FF, 2);
    cd_write(0x90024, 0xFFFF, 2);
    return (uint16_t)cd_read(0x90008, 2);
}

TEST(a_change_directory_ends_at_once_but_on_the_accurate_drive_only_once_the_drive_has_read_it) {
    char tmpl[] = "/tmp/cdrom_test_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    CHECK_EQ(cdrom_open(disc(dir, "fs", "DATA")), true);
    const uint16_t CMOK = 0x0001, EFLS = 0x0200;
    cd_init();
    CHECK_EQ(change_to_root() & (CMOK | EFLS), CMOK | EFLS);
    g_cfg.accurate_drive = true;
    cd_init();
    CHECK_EQ(change_to_root() & (CMOK | EFLS), CMOK);
    g_cfg.accurate_drive = false;
}

// The report's CD-ROM bit clears as a directory read starts its seek and is set again before the read ends.
TEST(a_directory_read_on_the_accurate_drive_clears_the_cd_rom_bit_until_its_first_sector) {
    char tmpl[] = "/tmp/cdrom_test_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    CHECK_EQ(cdrom_open(disc(dir, "bit", "DATA")), true);
    const uint16_t EFLS = 0x0200;
    g_cfg.accurate_drive = true;
    g_cfg.realtime = true;
    cd_init();
    change_to_root();
    auto bit = [] {
        cd_write(0x90018, 0x0000, 2), cd_write(0x9001C, 0, 2), cd_write(0x90020, 0, 2), cd_write(0x90024, 0, 2);
        uint16_t cr1 = (uint16_t)cd_read(0x90018, 2);
        cd_read(0x90024, 2);
        return cr1 & 0x80;
    };
    bool cleared = false, set_before_efls = false;
    for (int i = 0; i < 2000 && !(cd_read(0x90008, 2) & EFLS); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cd_tick();
        if (!bit()) cleared = true;
        else if (cleared && !(cd_read(0x90008, 2) & EFLS)) set_before_efls = true;
    }
    CHECK_EQ(cleared, true);
    CHECK_EQ(set_before_efls, true);
    CHECK_EQ(bit(), 0x80);
    g_cfg.accurate_drive = false;
    g_cfg.realtime = false;
}

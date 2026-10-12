// Borrowing a file from another disc: the directory leads to it, past this disc's lead-out.
#include "saturn.h"
#include "test.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

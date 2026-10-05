// The backup memory: saves written, read back by a later run, deleted, and kept where --save says.
#include "saturn.h"
#include "test.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

const uint32_t kDir = 0x06010000, kData = 0x06011000, kOut = 0x06012000, kName = 0x06013000;
const uint32_t kWrite = 4, kRead = 5, kDelete = 6, kDirList = 7;

fs::path scratch() {
    fs::path p = fs::temp_directory_path() / "saturn-bup-test";
    fs::remove_all(p);
    return p;
}

// A new run on a disc with this product number, saves from --save `save`.
void run(const std::string& save, const char* product = "T-1234H   ") {
    uint8_t ip[0x100] = {};
    std::memcpy(ip, "SEGA SEGASATURN ", 16);
    std::memcpy(ip + 0x20, product, 10);
    std::memcpy(ip + 0x2A, "V1.001", 6);
    g_cfg.save = save;
    bios_select_save(ip);
}

uint32_t bup(uint32_t k, uint32_t r4, uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0) {
    SH2Context& c = g_master;
    c.r[4] = r4; c.r[5] = r5; c.r[6] = r6; c.r[7] = r7;
    CHECK_EQ(bios_call(c, 0x00003000u + k * 4), 1);
    return c.r[0];
}

void name(uint32_t a, const char* s) {
    for (int i = 0; i < 12; ++i) st8(a + i, i < (int)std::strlen(s) ? s[i] : 0);
}

// Writes a save of `size` bytes, byte i being (i + seed), with r7 as `keep`.
uint32_t write(const char* save_name, uint32_t size, uint8_t seed, uint32_t keep = 0) {
    name(kDir, save_name);
    for (int i = 0; i < 11; ++i) st8(kDir + 12 + i, "a comment"[i < 9 ? i : 9]);
    st8(kDir + 23, 0);
    st32(kDir + 24, 1234);
    st32(kDir + 28, size);
    for (uint32_t i = 0; i < size; ++i) st8(kData + i, (uint8_t)(i + seed));
    return bup(kWrite, 0, kDir, kData, keep);
}

// 0 and the bytes as written, or BUP's not-found code.
uint32_t read_back(const char* save_name, uint32_t size, uint8_t seed) {
    name(kName, save_name);
    uint32_t r = bup(kRead, 0, kName, kOut);
    for (uint32_t i = 0; r == 0 && i < size; ++i) CHECK_EQ(ld8(kOut + i), (uint8_t)(i + seed));
    return r;
}

}  // namespace

TEST(a_save_is_read_back_by_the_next_run) {
    fs::path dir = scratch(), file = dir / "nested" / "backup.bin";
    run(file.string());
    CHECK_EQ(write("BOMB_TEST", 300, 7), 0);
    CHECK_EQ(fs::exists(file), 1);
    CHECK_EQ(fs::exists(file.string() + ".new"), 0);
    run(file.string());
    CHECK_EQ(read_back("BOMB_TEST", 300, 7), 0);
    name(kName, "BOMB");
    CHECK_EQ(bup(kDirList, 0, kName, 4, 0x06015000), 1);
    CHECK_EQ(ld32(0x06015000 + 28), 300);
}

TEST(saves_are_kept_by_name) {
    fs::path file = scratch() / "backup.bin";
    run(file.string());
    CHECK_EQ(write("FIRST", 10, 1), 0);
    CHECK_EQ(write("SECOND", 20, 2), 0);
    run(file.string());
    CHECK_EQ(read_back("FIRST", 10, 1), 0);
    CHECK_EQ(read_back("SECOND", 20, 2), 0);
    CHECK_EQ(read_back("THIRD", 0, 0), 5);
}

TEST(deleting_the_last_save_leaves_none) {
    fs::path file = scratch() / "backup.bin";
    run(file.string());
    CHECK_EQ(write("ONLY", 10, 1), 0);
    name(kName, "ONLY");
    CHECK_EQ(bup(kDelete, 0, kName), 0);
    run(file.string());
    CHECK_EQ(read_back("ONLY", 10, 1), 5);
}

TEST(a_run_that_saves_nothing_writes_no_file) {
    fs::path file = scratch() / "backup.bin";
    run(file.string());
    name(kName, "ANY");
    CHECK_EQ(bup(kRead, 0, kName, kOut), 5);
    bios_save();
    CHECK_EQ(fs::exists(file), 0);
}

TEST(save_dash_keeps_saves_for_the_run_alone) {
    fs::path dir = scratch();
    fs::create_directories(dir);
    setenv("XDG_DATA_HOME", dir.c_str(), 1);
    run("-");
    CHECK_EQ(write("RUN_ONLY", 10, 1), 0);
    CHECK_EQ(read_back("RUN_ONLY", 10, 1), 0);
    CHECK_EQ(fs::is_empty(dir), 1);
    run("-");
    CHECK_EQ(read_back("RUN_ONLY", 10, 1), 5);
}

TEST(saves_default_to_a_folder_per_disc_in_the_data_directory) {
    fs::path dir = scratch();
    setenv("XDG_DATA_HOME", dir.c_str(), 1);
    run("");
    CHECK_EQ(write("DEFAULT", 10, 3), 0);
    CHECK_EQ(fs::exists(dir / "saturn-recomp" / "T-1234H_V1.001" / "backup.bin"), 1);
    run("", "T-9999H   ");
    CHECK_EQ(read_back("DEFAULT", 10, 3), 5);
    run("");
    CHECK_EQ(read_back("DEFAULT", 10, 3), 0);
}

TEST(a_damaged_file_keeps_the_saves_before_the_damage) {
    fs::path file = scratch() / "backup.bin";
    run(file.string());
    CHECK_EQ(write("GOOD", 10, 1), 0);
    CHECK_EQ(write("LATER", 4000, 2), 0);
    fs::resize_file(file, fs::file_size(file) - 100);
    run(file.string());
    CHECK_EQ(read_back("GOOD", 10, 1), 0);
    CHECK_EQ(read_back("LATER", 4000, 2), 5);
}

TEST(a_write_over_a_save_needs_r7_zero) {
    fs::path file = scratch() / "backup.bin";
    run(file.string());
    CHECK_EQ(write("SAME", 10, 1), 0);
    CHECK_EQ(write("SAME", 10, 2, 2), 6);
    CHECK_EQ(read_back("SAME", 10, 1), 0);
    CHECK_EQ(write("SAME", 10, 3), 0);
    CHECK_EQ(read_back("SAME", 10, 3), 0);
}

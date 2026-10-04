// VDP2 composition from registers, VRAM and colour RAM set by hand.
#include "test.h"
#include "video.h"
#include <cstring>

namespace {

const uint32_t kRed = 0xF80000, kGreen = 0x00F800, kBlue = 0x0000F8;   // RGB555 0x001F, 0x03E0, 0x7C00

void reset() {
    std::memset(g_vdp2_regs, 0, sizeof g_vdp2_regs);
    std::memset(g_vdp2_vram, 0, sizeof g_vdp2_vram);
    std::memset(g_vdp2_cram, 0, sizeof g_vdp2_cram);
}

void reg(uint32_t off, uint16_t v) { g_vdp2_regs[off] = v >> 8; g_vdp2_regs[off + 1] = (uint8_t)v; }
void vram16(uint32_t a, uint16_t v) { g_vdp2_vram[a] = v >> 8; g_vdp2_vram[a + 1] = (uint8_t)v; }
void cram16(uint32_t i, uint16_t v) { g_vdp2_cram[i * 2] = v >> 8; g_vdp2_cram[i * 2 + 1] = (uint8_t)v; }

uint32_t at(const Frame& f, int x, int y) { return f.px[(size_t)y * f.w + x]; }

// 320x224 with the display on, and a red back screen at VRAM 0x40000.
void red_screen() {
    reset();
    reg(0x00, 0x8000);                           // TVMD: display on
    reg(0xAC, 0x0002);                           // BKTAU/BKTAL: back screen at word 0x20000
    vram16(0x40000, 0x001F);
}

// NBG0 as a 512x256 bitmap of 256 colours at VRAM 0, every dot colour 1 (green), priority 1.
void green_bitmap_nbg0() {
    reg(0x20, 0x0001);                           // BGON: NBG0
    reg(0x28, 0x0012);                           // CHCTLA: NBG0 bitmap, 256 colours, 512x256
    reg(0x78, 0x0001);                           // ZMXIN0: X increment 1.0
    reg(0x7C, 0x0001);                           // ZMYIN0: Y increment 1.0
    reg(0xF8, 0x0001);                           // PRINA: NBG0 priority 1
    std::memset(g_vdp2_vram, 1, 512 * 256);
    cram16(1, 0x03E0);
}

}  // namespace

TEST(display_off_is_black) {
    reset();
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(f.w, 320);
    CHECK_EQ(f.h, 224);
    CHECK_EQ(at(f, 100, 100), 0);
}

TEST(back_screen_shows_with_no_layers) {
    red_screen();
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 0, 0), kRed);
    CHECK_EQ(at(f, 319, 223), kRed);
}

TEST(bitmap_dot_zero_is_transparent) {
    red_screen();
    green_bitmap_nbg0();
    g_vdp2_vram[5] = 0;                          // dot (5, 0)
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 4, 0), kGreen);
    CHECK_EQ(at(f, 5, 0), kRed);
    CHECK_EQ(at(f, 5, 1), kGreen);
}

TEST(window_inside_hides_the_layer) {
    red_screen();
    green_bitmap_nbg0();
    reg(0xC4, 38);                               // WPEX0: X 0-38 half dots, dots 0-19
    reg(0xC6, 223);                              // WPEY0
    reg(0xD0, 0x0002);                           // WCTLA: NBG0 uses window 0, transparent inside
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 100), kRed);
    CHECK_EQ(at(f, 19, 100), kRed);
    CHECK_EQ(at(f, 20, 100), kGreen);
}

TEST(window_outside_hides_the_rest) {
    red_screen();
    green_bitmap_nbg0();
    reg(0xC4, 38);
    reg(0xC6, 223);
    reg(0xD0, 0x0003);                           // WCTLA: window 0, transparent outside
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 100), kGreen);
    CHECK_EQ(at(f, 20, 100), kRed);
}

TEST(colour_calculation_mixes_by_ratio) {
    red_screen();
    green_bitmap_nbg0();
    reg(0xEC, 0x0001);                           // CCCTL: NBG0
    reg(0x108, 16);                              // CCRNA: NBG0 ratio 16 of 32
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 0, 0), 0x7C7C00);
}

TEST(higher_priority_wins_and_nbg0_wins_ties) {
    red_screen();
    green_bitmap_nbg0();
    reg(0x20, 0x0003);                           // BGON: NBG0, NBG1
    reg(0x28, 0x1212);                           // CHCTLA: both bitmaps of 256 colours
    reg(0x3C, 0x0010);                           // MPOFN: NBG1's bitmap at 0x20000
    reg(0x88, 0x0001);                           // ZMXIN1
    reg(0x8C, 0x0001);                           // ZMYIN1
    std::memset(g_vdp2_vram + 0x20000, 2, 512 * 256);
    cram16(2, 0x7C00);
    Frame f;
    reg(0xF8, 0x0201);                           // NBG1 2 over NBG0 1
    vdp2_compose(f);
    CHECK_EQ(at(f, 0, 0), kBlue);
    reg(0xF8, 0x0103);                           // NBG0 3 over NBG1 1
    vdp2_compose(f);
    CHECK_EQ(at(f, 0, 0), kGreen);
    reg(0xF8, 0x0202);                           // a tie
    vdp2_compose(f);
    CHECK_EQ(at(f, 0, 0), kGreen);
}

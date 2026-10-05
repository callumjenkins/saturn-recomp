// VDP2 composition from registers, VRAM and colour RAM set by hand.
#include "test.h"
#include "video.h"
#include <cstring>

namespace {

const uint32_t kRed = 0xF80000, kGreen = 0x00F800, kBlue = 0x0000F8;   // RGB555 0x001F, 0x03E0, 0x7C00

// VDP1 shows dot d in columns x0 to x1 of every line, and nothing elsewhere.
void show_sprites(uint16_t d = 0, int x0 = 0, int x1 = -1) {
    for (uint32_t i = 0; i < 512 * 256; ++i) vdp1_fb_write(i * 2, i % 512 >= (uint32_t)x0 && i % 512 <= (uint32_t)x1 ? d : 0, 2);
    vdp1_vblank_out();
}

void reset() {
    std::memset(g_vdp2_regs, 0, sizeof g_vdp2_regs);
    std::memset(g_vdp2_vram, 0, sizeof g_vdp2_vram);
    std::memset(g_vdp2_cram, 0, sizeof g_vdp2_cram);
    show_sprites();
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

// The sprite layer as type 2 (priority bit 14, colour bits 0-10) at priority 3, over green NBG0 at 1.
void sprites_over_green() {
    red_screen();
    green_bitmap_nbg0();
    reg(0xE0, 0x0002);                           // SPCTL: type 2
    reg(0xF0, 0x0003);                           // PRISA: sprite priority 0 is 3
}

TEST(normal_shadow_halves_an_enabled_screen) {
    sprites_over_green();
    show_sprites(0x07FE, 10, 19);                // every colour bit 1 but the lowest
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), kGreen);              // SDCTL enables no screen
    reg(0xE2, 0x0001);                           // SDCTL: NBG0
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), 0x007C00);
    CHECK_EQ(at(f, 20, 0), kGreen);
    reg(0x20, 0x0000);                           // BGON: nothing, so the back screen is on top
    reg(0xE2, 0x0020);                           // SDCTL: the back screen
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), 0x7C0000);
}

TEST(shadow_needs_a_priority_over_the_screen) {
    sprites_over_green();
    reg(0xE2, 0x0001);
    reg(0xF8, 0x0004);                           // PRINA: NBG0 4, over the shadow's 3
    show_sprites(0x07FE, 10, 19);
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), kGreen);
}

TEST(msb_shadow_halves_the_sprite_itself) {
    sprites_over_green();
    cram16(2, 0x7C00);
    show_sprites(0x8002, 10, 19);
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), 0x00007C);
    reg(0xE0, 0x0012);                           // SPCTL: SPWINEN, so the MSB is the sprite window's
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), kBlue);
}

TEST(transparent_shadow_needs_tpsdsl) {
    sprites_over_green();
    reg(0xE2, 0x0001);
    show_sprites(0x8000, 10, 19);
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), kGreen);
    reg(0xE2, 0x0101);                           // SDCTL: TPSDSL and NBG0
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), 0x007C00);
}

// Green NBG0 mixed half and half, over a line colour table at VRAM 0x50000.
void line_colour_over_green() {
    red_screen();
    green_bitmap_nbg0();
    reg(0xEC, 0x0001);                           // CCCTL: NBG0
    reg(0x108, 16);                              // CCRNA: NBG0 ratio 16 of 32
    reg(0xAA, 0x8000);                           // LCTAL: the table at word 0x28000
    reg(0xA8, 0x0002);                           // LCTAU: one colour for the screen
    vram16(0x50000, 3);                          // the colour RAM entry for it
    cram16(3, 0x7C00);
}

TEST(line_colour_is_the_second_image_where_lnclen_names_the_top) {
    line_colour_over_green();
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 0, 0), 0x7C7C00);             // LNCLEN off: green with the red back screen
    reg(0xE8, 0x0001);                           // LNCLEN: NBG0
    vdp2_compose(f);
    CHECK_EQ(at(f, 0, 0), 0x007C7C);             // green with the blue line colour
}

TEST(line_colour_a_line) {
    line_colour_over_green();
    reg(0xE8, 0x0001);
    reg(0xA8, 0x8002);                           // LCTAU: LCCLMD, a colour a line
    vram16(0x50002, 4);                          // line 1's entry
    cram16(4, 0x001F);
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 0, 0), 0x007C7C);
    CHECK_EQ(at(f, 0, 1), 0x7C7C00);             // line 1: red
}

// Green NBG1 mixed half and half over NBG0, whose bitmap is blue for x 0-9 and black from x 10.
void green_over_blue_then_black() {
    red_screen();
    reg(0x20, 0x0003);                           // BGON: NBG0, NBG1
    reg(0x28, 0x1212);                           // CHCTLA: both bitmaps of 256 colours
    reg(0x3C, 0x0010);                           // MPOFN: NBG1's bitmap at 0x20000
    reg(0x78, 0x0001); reg(0x7C, 0x0001);        // ZMXIN0, ZMYIN0
    reg(0x88, 0x0001); reg(0x8C, 0x0001);        // ZMXIN1, ZMYIN1
    reg(0xF8, 0x0201);                           // PRINA: NBG1 2 over NBG0 1
    reg(0x108, 0x1000);                          // CCRNA: NBG1 ratio 16 of 32
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 512; ++x) g_vdp2_vram[y * 512 + x] = x < 10 ? 2 : 5;
    std::memset(g_vdp2_vram + 0x20000, 1, 512 * 256);
    cram16(1, 0x03E0);
    cram16(2, 0x7C00);
    cram16(5, 0x0000);
}

TEST(gradation_blurs_the_second_image_along_the_line) {
    green_over_blue_then_black();
    reg(0xEC, 0x0002);                           // CCCTL: NBG1
    Frame f;
    vdp2_compose(f);
    CHECK_EQ(at(f, 10, 0), 0x007C00);            // no gradation: half green, half black
    reg(0xEC, 0xA002);                           // CCCTL: BOKEN, BOKN NBG0, and NBG1
    vdp2_compose(f);
    CHECK_EQ(at(f, 5, 0), 0x007C7C);             // all blue around it
    CHECK_EQ(at(f, 10, 0), 0x007C3E);            // blue, blue, black: half blue
    CHECK_EQ(at(f, 11, 0), 0x007C1F);            // blue, black, black: a quarter
    CHECK_EQ(at(f, 12, 0), 0x007C00);
}

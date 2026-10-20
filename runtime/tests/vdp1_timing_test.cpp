// How long VDP1 takes over a draw: the sprite-draw-end comes once the cycles its commands cost have
// gone by at the SH-2's clock. With SATURN_VDP1_GPL the costs are Mednafen's rules, counted by hand
// below; without it, the fit in vdp1_raster.inc.
#include "saturn.h"
#include "test.h"
#include "video.h"
#include <cstring>

namespace {

const int kTVMR = 0, kPTMR = 4, kEDSR = 0x10;

void put16(uint32_t a, uint16_t v) { g_vdp1_vram[a] = v >> 8; g_vdp1_vram[a + 1] = (uint8_t)v; }

// Each list sets the system clipping to 319 x 223 and the local coordinates to 0, then draws the
// shape at 0x40 and ends.
const uint64_t kFetches = 4 * 16;

void list() {
    std::memset(g_vdp1_vram, 0, 0x2000);
    put16(0x00, 0x0009);
    put16(0x14, 319);
    put16(0x16, 223);
    put16(0x20, 0x000A);
    put16(0x60, 0x8000);
}

// A normal sprite, w x 8 dots of 4-bit colour at (x, 10), its texture at 0x1000.
void sprite(int w, int x, uint16_t pmod) {
    list();
    put16(0x44, pmod);
    put16(0x48, 0x1000 / 8);
    put16(0x4A, (uint16_t)((w / 8) << 8 | 8));
    put16(0x4C, (uint16_t)x);
    put16(0x4E, 10);
    put16(0x5C, 0x1800 / 8);                    // a Gouraud table, all neutral
    for (int i = 0; i < 4; ++i) put16(0x1800 + i * 2, 0x4210);
}

// Command `comm` with corners A to D, an 8 x 8 texture at 0x1000 for a sprite.
void shape(uint16_t comm, const int (&xy)[8], uint16_t pmod) {
    sprite(8, 0, pmod);
    put16(0x40, comm);
    for (int i = 0; i < 8; ++i) put16((uint32_t)(0x4C + i * 2), (uint16_t)xy[i]);
}

// The square (10, 10) to (17, 17), or a line along its top.
const int kSquare[8] = {10, 10, 17, 10, 17, 17, 10, 17};
const int kTop[8] = {10, 10, 17, 10, 0, 0, 0, 0};

bool ended() { return vdp1_reg_read(kEDSR, 2) & 2; }

// Draws, and checks it ends `cycles` later and not a cycle sooner.
void check_ends_after(uint64_t cycles, uint16_t tvmr = 0) {
    vdp1_reg_write(kTVMR, tvmr, 2);
    uint64_t now = sat_now();
    vdp1_reg_write(kPTMR, 1, 2);
    vdp1_tick(now + onchip_phi_ns(cycles - 1));
    CHECK_EQ(ended(), false);
    vdp1_tick(now + onchip_phi_ns(cycles));
    CHECK_EQ(ended(), true);
    vdp1_reg_write(kPTMR, 0, 2);
    vdp1_reg_write(kTVMR, 0, 2);
}

}  // namespace

#if SATURN_VDP1_GPL

// 8 lines, each 12 to set up and 8 pixels at 1, and 48/256 of the 64 pixels' cycles: 12 more.
TEST(a_sprite_costs_its_lines_and_pixels) {
    sprite(8, 10, 0);
    check_ends_after(kFetches + 8 * 12 + 64 + 12);
}

TEST(a_line_costs_4_cycles_less_without_pre_clipping) {
    sprite(8, 10, 0x0800);
    check_ends_after(kFetches + 8 * 8 + 64 + 12);
}

// MSB on reads the framebuffer: 6 a pixel, 384 in all, and 72 more.
TEST(a_pixel_that_reads_the_framebuffer_costs_6) {
    sprite(8, 10, 0x8000);
    check_ends_after(kFetches + 8 * 12 + 384 + 72);
}

// Every line is wholly outside the system clipping: set up, and the last drawn as a point.
TEST(a_sprite_outside_the_clipping_costs_its_lines_set_up) {
    sprite(8, 400, 0);
    check_ends_after(kFetches + 8 * 12 + 1);
}

TEST(at_8_bits_a_pixel_the_pixels_cost_24_256ths_more) {
    sprite(8, 10, 0);
    check_ends_after(kFetches + 8 * 12 + 64 + 6, 1);
}

TEST(a_lookup_table_costs_16_and_a_gouraud_table_4) {
    sprite(8, 10, 0x0008);
    check_ends_after(kFetches + 16 + 8 * 12 + 64 + 12);
    sprite(8, 10, 0x0004);
    check_ends_after(kFetches + 4 + 8 * 12 + 64 + 12);
}

TEST(shadow_and_half_transparency_pixels_cost_6_and_half_luminance_1) {
    sprite(8, 10, 0x0001);
    check_ends_after(kFetches + 8 * 12 + 384 + 72);
    sprite(8, 10, 0x0003);
    check_ends_after(kFetches + 8 * 12 + 384 + 72);
    sprite(8, 10, 0x0002);
    check_ends_after(kFetches + 8 * 12 + 64 + 12);
}

// 4 of each row's 8 pixels are inside; the line stops at the first one outside, which costs nothing.
TEST(a_line_leaving_the_clipping_stops_costing_there) {
    sprite(8, 316, 0);
    check_ends_after(kFetches + 8 * 12 + 32 + 6);
}

TEST(a_skipped_command_costs_its_fetch) {
    sprite(8, 10, 0);
    put16(0x40, 0x4000);
    check_ends_after(kFetches);
}

// A line command costs 1, and its one line 12 and 8 pixels, with a cycle more for them.
TEST(a_line_command_costs_1_and_its_line) {
    shape(0x0006, kTop, 0);
    check_ends_after(kFetches + 1 + 12 + 8 + 1);
    shape(0x0006, kTop, 0x0004);
    check_ends_after(kFetches + 2 + 1 + 12 + 8 + 1);
}

// Each draw starts without the part of a cycle the one before left over: here 128/256.
TEST(the_part_of_a_cycle_left_over_starts_again_at_each_draw) {
    shape(0x0006, kTop, 0);
    check_ends_after(kFetches + 1 + 12 + 8 + 1);
    check_ends_after(kFetches + 1 + 12 + 8 + 1);
}

// Four lines of 12 and 8 pixels, 6 more for their pixels, and 2 a line with Gouraud.
TEST(a_polyline_costs_1_and_its_four_lines) {
    shape(0x0005, kSquare, 0);
    check_ends_after(kFetches + 1 + 4 * 12 + 32 + 6);
    shape(0x0005, kSquare, 0x0004);
    check_ends_after(kFetches + 8 + 1 + 4 * 12 + 32 + 6);
}

TEST(a_polygon_costs_its_lines_and_4_for_gouraud_but_nothing_for_a_lookup_table) {
    shape(0x0004, kSquare, 0);
    check_ends_after(kFetches + 8 * 12 + 64 + 12);
    shape(0x0004, kSquare, 0x0004);
    check_ends_after(kFetches + 4 + 8 * 12 + 64 + 12);
    shape(0x0004, kSquare, 0x0008);
    check_ends_after(kFetches + 8 * 12 + 64 + 12);
}

TEST(a_distorted_sprite_with_a_lookup_table_costs_16_more) {
    shape(0x0002, kSquare, 0x0008);
    check_ends_after(kFetches + 16 + 8 * 12 + 64 + 12);
}

#else

// 8 rows of 8 cycles and 8 dots at 19/16.
TEST(a_sprite_costs_its_rows_and_dots) {
    sprite(8, 10, 0);
    check_ends_after(kFetches + 140);
}

// A row ends where it leaves the clip window: 4 of the 8 dots, at x 316 to 319.
TEST(a_sprite_row_stops_at_the_clip_window_edge) {
    sprite(8, 316, 0);
    check_ends_after(kFetches + 102);
}

// Rows of 12 cycles and 16 dots at 19/16.
TEST(a_wide_sprite_s_rows_cost_12) {
    sprite(16, 10, 0);
    check_ends_after(kFetches + 248);
}

TEST(a_sprite_starting_past_the_clip_window_is_walked_in_full) {
    sprite(8, 400, 0);
    check_ends_after(kFetches + 140);
}

// User clipping to x 13: 4 dots a row.
TEST(user_clipping_narrows_the_walk) {
    sprite(8, 10, 0x0400);
    put16(0x00, 0x0008);                        // the system clipping's command becomes user clipping
    put16(0x0C, 0);
    put16(0x0E, 0);
    put16(0x14, 13);
    put16(0x16, 223);
    check_ends_after(kFetches + 102);
    put16(0x14, 319);
    check_ends_after(kFetches + 140);
}

// 8 rows of 13.3, and 16 x 8 dots at 1.1.
TEST(a_scaled_sprite_costs_its_rows_and_the_dots_inside) {
    shape(0x0001, {10, 10, 0, 0, 25, 17, 0, 0}, 0);
    check_ends_after(kFetches + 247);
}

// 8 lines of 7.6 and 8 dots at 1.6; a distorted sprite's 9.8 and 1.35.
TEST(polygons_and_distorted_sprites_cost_their_lines_and_dots) {
    shape(0x0004, kSquare, 0);
    check_ends_after(kFetches + 163);
    shape(0x0002, kSquare, 0);
    check_ends_after(kFetches + 165);
}

// x 316 to 323: each line's dots stop at 319, 4 of them.
TEST(a_polygon_line_stops_where_it_leaves_the_clip_window) {
    shape(0x0004, {316, 10, 323, 10, 323, 17, 316, 17}, 0);
    check_ends_after(kFetches + 112);
}

TEST(lines_and_polylines_cost_as_a_polygon_s_lines) {
    shape(0x0006, kTop, 0);
    check_ends_after(kFetches + 20);
    shape(0x0005, kSquare, 0);
    check_ends_after(kFetches + 82);
}

#endif

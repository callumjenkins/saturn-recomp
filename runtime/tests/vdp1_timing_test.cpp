// How long VDP1 takes over a draw: the sprite-draw-end comes when the command table's cycles,
// at 28.64 MHz, have gone by. The expected cycles are what Beetle Saturn charges.
#include "saturn.h"
#include "test.h"
#include "video.h"
#include <cstring>

namespace {

const int kPTMR = 4, kEDSR = 0x10;

void put16(uint32_t a, uint16_t v) { g_vdp1_vram[a] = v >> 8; g_vdp1_vram[a + 1] = (uint8_t)v; }

// A normal sprite, w x 8 dots of 4-bit colour at (x, 10), then the END command.
void sprite_then_end(int w, int x) {
    std::memset(g_vdp1_vram, 0, 0x40);
    put16(0x0A, (uint16_t)((w / 8) << 8 | 8));
    put16(0x0C, (uint16_t)x);
    put16(0x0E, 10);
    put16(0x20, 0x8000);
}

bool ended() { return vdp1_reg_read(kEDSR, 2) & 2; }

// Draws now, at machine time `now`, and says whether it had ended one nanosecond before `ns`
// later, and at `ns`.
void check_ends_after(uint64_t now, uint64_t ns) {
    vdp1_reg_write(kPTMR, 1, 2);
    vdp1_tick(now + ns - 1);
    CHECK_EQ(ended(), false);
    vdp1_tick(now + ns);
    CHECK_EQ(ended(), true);
    vdp1_reg_write(kPTMR, 0, 2);
}

}  // namespace

// 16 to fetch each command, and 8 rows of 8 cycles and 8 dots at 19/16: 172 cycles.
TEST(an_8_dot_sprite_takes_its_rows_and_dots) {
    sprite_then_end(8, 10);
    check_ends_after(sat_now(), 6006);
}

// A row ends where it leaves the clip window: 4 of the 8 dots at x 316 to 319.
TEST(a_sprite_row_stops_at_the_clip_window_edge) {
    sprite_then_end(8, 316);
    check_ends_after(sat_now(), (uint64_t)((16 + 8 * (8 + 4 * 19.0 / 16) + 16) * 1e9 / 28636360.0));
}

// When a manual VDP1 frame change asked for by FBCR happens, against the raster.
#include "saturn.h"
#include "test.h"
#include "video.h"

namespace {

const uint64_t kLineNs = 16683350 / 263;
const int kFBCR = 2;

// Machine time at a line of a frame, counted from power-on.
uint64_t at(uint64_t frame, int line) { return (frame * 263 + line) * kLineNs; }

// Each tick stops at VBlank-OUT, so a run across several frames takes a tick for each.
void run_to(uint64_t now) {
    for (int i = 0; i < 1000; ++i) video_tick(now);
}

// Time only goes forward, so each test starts at a frame no test has reached.
uint64_t fresh_frame() {
    static uint64_t frame = 0;
    return frame += 10;
}

// One-cycle mode, as the other tests expect VDP1 to be.
struct Restore { ~Restore() { vdp1_reg_write(kFBCR, 0, 2); } };

}  // namespace

// A poll's time can cover VBlank-OUT and the line after it. The change is decided at that line, so a
// write from the VBlank-OUT handler, taken at the same poll, has to come first.
TEST(the_vblank_out_handler_asks_in_time_for_that_blanking) {
    Restore restore;
    uint64_t f = fresh_frame();
    vdp1_reg_write(kFBCR, 3, 2);
    run_to(at(f, 5));
    uint64_t changes = vdp1_frame_changes();
    video_tick(at(f + 1, 2));
    vdp1_reg_write(kFBCR, 3, 2);
    video_tick(at(f + 1, 2));
    CHECK_EQ(vdp1_frame_changes(), changes + 1);
}

TEST(a_change_asked_for_during_the_frame_comes_at_the_next_blanking_only) {
    Restore restore;
    uint64_t f = fresh_frame();
    vdp1_reg_write(kFBCR, 3, 2);
    run_to(at(f, 5));
    uint64_t changes = vdp1_frame_changes();
    vdp1_reg_write(kFBCR, 3, 2);
    run_to(at(f + 1, 0) - 1);
    CHECK_EQ(vdp1_frame_changes(), changes);
    run_to(at(f + 1, 2));
    CHECK_EQ(vdp1_frame_changes(), changes + 1);
    run_to(at(f + 2, 2));
    CHECK_EQ(vdp1_frame_changes(), changes + 1);
}

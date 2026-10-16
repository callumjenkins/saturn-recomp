// When a manual VDP1 frame change asked for by FBCR happens, against the raster.
#include "saturn.h"
#include "test.h"
#include "video.h"

namespace {

const uint64_t kLineNs = video_frame_ns() / 263;
const int kFBCR = 2;

// Machine time at a line of a frame, counted from power-on.
uint64_t at(uint64_t frame, int line) { return (frame * 263 + line) * kLineNs; }

void run_to(uint64_t now) { video_tick(now); }

// Time only goes forward, so each test starts at a frame no test has reached.
uint64_t fresh_frame() {
    static uint64_t frame = 0;
    return frame += 10;
}

// One-cycle mode, as the other tests expect VDP1 to be.
struct Restore { ~Restore() { vdp1_reg_write(kFBCR, 0, 2); } };

}  // namespace

// The change is decided as VBlank-OUT begins, before its handler can run, so a request the handler makes
// waits for the next blanking.
TEST(a_request_from_the_vblank_out_handler_waits_for_the_next_blanking) {
    Restore restore;
    uint64_t f = fresh_frame();
    vdp1_reg_write(kFBCR, 3, 2);
    run_to(at(f, 5));
    uint64_t changes = vdp1_frame_changes();
    run_to(at(f + 1, 0));
    vdp1_reg_write(kFBCR, 3, 2);
    run_to(at(f + 1, 2));
    CHECK_EQ(vdp1_frame_changes(), changes);
    run_to(at(f + 2, 0));
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

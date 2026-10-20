// When a manual VDP1 frame change asked for by FBCR happens, against the raster.
#include "saturn.h"
#include "test.h"
#include "video.h"

namespace {

const uint64_t kLineNs = video_frame_ns() / 263;
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

// Time can pass line 0 and line 1 at once, as a poll in real time does. The raster stops at line 0, so
// the VBlank-OUT handler's write still comes before line 1 decides the change.
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

TEST(the_next_blanking_line_is_line_0_or_line_1) {
    CHECK_EQ(video_next_blanking_line(at(5, 100)), at(6, 0));
    CHECK_EQ(video_next_blanking_line(at(6, 0) - 1), at(6, 0));
    CHECK_EQ(video_next_blanking_line(at(6, 0)), at(6, 1));
    CHECK_EQ(video_next_blanking_line(at(6, 0) + 1), at(6, 1));
    CHECK_EQ(video_next_blanking_line(at(6, 1)), at(7, 0));
}

TEST(polls_land_on_line_0_and_line_1) {
    SH2Context c{};
    c.cpu = 0;
    c.imask = 15;
    sh2_poll(c);
    uint64_t frame = sat_now() / video_frame_ns() + 1;
    bool on_line_0 = false, on_line_1 = false;
    while (sat_now() < at(frame, 2)) {
        uint64_t before = sat_now();
        int32_t budget = c.budget;
        CHECK_EQ(budget >= 1 && budget <= 256, true);
        sh2_poll(c);
        CHECK_EQ(sat_now() - before, (uint64_t)budget * 400);
        on_line_0 |= sat_now() >= at(frame, 0) && sat_now() < at(frame, 0) + 400;
        on_line_1 |= sat_now() >= at(frame, 1) && sat_now() < at(frame, 1) + 400;
    }
    CHECK_EQ(on_line_0, true);
    CHECK_EQ(on_line_1, true);
}

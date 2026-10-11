#include "pace.h"
#include "test.h"

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

uint64_t ns(PaceClock::duration d) { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(d).count(); }

}  // namespace

TEST(a_vblank_waits_for_its_time) {
    PaceClock::time_point base{};
    CHECK_EQ(ns(pace_wait(base, ns(milliseconds(17)), base + milliseconds(10))), ns(milliseconds(7)));
    CHECK_EQ(ns(pace_wait(base, ns(milliseconds(17)), base + milliseconds(20))), 0);
}

TEST(a_loaded_dump_goes_on_at_once) {
    PaceClock::time_point start{};
    PaceClock::time_point base = start, t = start + seconds(3);
    CHECK_EQ(ns(pace_wait(base, ns(seconds(4316)), t)), 0);
    CHECK_EQ(ns(pace_wait(base, ns(seconds(4316) + milliseconds(17)), t)), ns(milliseconds(17)));
}

TEST(a_stall_is_not_run_to_catch_up) {
    PaceClock::time_point base{};
    PaceClock::time_point t = base + seconds(5);
    CHECK_EQ(ns(pace_wait(base, ns(milliseconds(17)), t)), 0);
    CHECK_EQ(ns(pace_wait(base, ns(milliseconds(34)), t)), ns(milliseconds(17)));
}

TEST(exactly_100_ms_of_drift_preserves_the_schedule) {
    PaceClock::time_point start{}, base = start;
    CHECK_EQ(ns(pace_wait(base, ns(milliseconds(100)), start)), ns(milliseconds(100)));
    CHECK_EQ(ns(base - start), 0);
    CHECK_EQ(ns(pace_wait(base, 0, start + milliseconds(100))), 0);
    CHECK_EQ(ns(base - start), 0);
}

TEST(small_lateness_preserves_the_next_due_time) {
    PaceClock::time_point start{}, base = start, t = start + milliseconds(20);
    CHECK_EQ(ns(pace_wait(base, ns(milliseconds(17)), t)), 0);
    CHECK_EQ(ns(base - start), 0);
    CHECK_EQ(ns(pace_wait(base, ns(milliseconds(34)), t)), ns(milliseconds(14)));
}

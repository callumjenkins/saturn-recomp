// saturn-recomp runtime — the window's pace: how long the host waits for each VBlank to come due.
#pragma once
#include <chrono>
#include <cstdint>

using PaceClock = std::chrono::steady_clock;

// How long to wait at host time `t` for machine time `now`, given `base`, the host time of machine time 0.
// More than 100 ms out either way moves `base` so the machine goes on from `t`.
inline PaceClock::duration pace_wait(PaceClock::time_point& base, uint64_t now, PaceClock::time_point t) {
    const auto slack = std::chrono::milliseconds(100);
    PaceClock::time_point due = base + std::chrono::nanoseconds(now);
    if (t > due + slack || due > t + slack) {
        base += t - due;
        return {};
    }
    return due > t ? due - t : PaceClock::duration{};
}

#include "saturn.h"
#include "test.h"
#include "saturn/sh2.h"

// The expected values are a * b // c in Python's integers.
TEST(mul_div_is_exact_including_products_past_64_bits) {
    CHECK_EQ(sat_mul_div(1000000000ull, 1746818182u, 65000000000ull), 26874125ull);
    CHECK_EQ(sat_mul_div(9223372036854788153ull, 1746818182u, 65000000000ull), 247870061128127969ull);
    CHECK_EQ(sat_mul_div(18446744073709551615ull, 1734687500u, 61000000000ull), 524579284596115374ull);
    CHECK_EQ(sat_mul_div(123456789012345ull, 1746818182u, 61000000000ull), 3535353503903ull);
    CHECK_EQ(sat_mul_div(18446744073709551615ull, 4294967295u, 140737488355327ull), 562949953290243ull);
}

// Mednafen's NTSC master clock is 1 746 818 182 Hz: φ is a 65th of it at 320 dots and a 61st at 352.
TEST(the_on_chip_timers_count_at_320_dots_rate_until_smpc_changes_the_clock) {
    onchip_reset(0);
    CHECK_EQ(onchip_phi_clocks(1000000000ull), 26874125ull);
    smpc_write(0x1F, 0x0E);                     // CKCHG352
    CHECK_EQ(onchip_phi_clocks(1000000000ull), 28636363ull);
    smpc_write(0x1F, 0x0F);                     // CKCHG320
    CHECK_EQ(onchip_phi_clocks(1000000000ull), 26874125ull);
}

// The expected values are clocks * 65e9 // master, and * 61e9 at 352 dots.
TEST(the_cycles_of_phi_take_its_time_at_either_dot_clock) {
    onchip_reset(0);
    CHECK_EQ(onchip_phi_ns(26874125ull), 999999967ull);
    CHECK_EQ(onchip_phi_ns(1000ull), 37210ull);
    smpc_write(0x1F, 0x0E);                     // CKCHG352
    CHECK_EQ(onchip_phi_ns(28636363ull), 999999977ull);
    smpc_write(0x1F, 0x0F);
}

namespace {
// The time one master poll charges, from the virtual time, which the frame-change tests need short of
// their first frame: with `cycles` more of the master's and `slave` more of the slave's
uint64_t polled(uint64_t cycles, uint64_t slave = 0) {
    g_cpu = &g_master;
    g_master.imask = 15;                        // no interrupt handler runs
    g_sh2_cycles[0] += cycles;
    g_sh2_cycles[1] += slave;
    uint64_t t0 = sat_now();
    sh2_poll(g_master);
    return sat_now() - t0;
}
}  // namespace

TEST(the_default_pace_charges_400_ns_a_safe_point_whatever_the_cycles) {
    onchip_reset(0);
    g_cfg.accurate_cpu = false;
    CHECK_EQ(polled(100000), 256 * 400);
    CHECK_EQ(g_master.budget, 256);
}

TEST(the_accurate_pace_charges_the_masters_cycles_at_phi_and_polls_every_32_safe_points) {
    onchip_reset(0);
    g_cfg.accurate_cpu = true;
    polled(0);
    uint64_t ns = polled(268741);               // 10 ms of φ: 9999990.4 ns
    CHECK_EQ(ns >= 9999990 && ns <= 9999991, true);
    CHECK_EQ(g_master.budget, 32);
    CHECK_EQ(polled(0, 1000000), 0);            // the slave's cycles are not the master's time
    g_cfg.accurate_cpu = false;
}

TEST(the_accurate_pace_loses_no_rounding_from_poll_to_poll) {
    onchip_reset(0);
    g_cfg.accurate_cpu = true;
    polled(0);
    uint64_t one_at_a_time = 0;
    for (int i = 0; i < 1000; ++i) one_at_a_time += polled(1);
    CHECK_EQ(one_at_a_time >= 37209 && one_at_a_time <= 37211, true);   // 1000 cycles' 37210.4 ns
    g_cfg.accurate_cpu = false;
}

TEST(a_register_access_costs_the_cpu_8_cycles) {
    g_cpu = &g_master;
    uint64_t before = g_sh2_cycles[0];
    sh2_mmio_read(0x05FE00A4, 4);               // the SCU's IST
    CHECK_EQ(g_sh2_cycles[0] - before, 8);
}

#include "saturn.h"
#include "test.h"

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

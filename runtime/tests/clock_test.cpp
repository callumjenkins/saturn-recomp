#include "saturn.h"
#include "test.h"

// Mednafen's NTSC master clock is 1 746 818 182 Hz: φ is a 65th of it at 320 dots and a 61st at 352.
TEST(the_on_chip_timers_count_at_320_dots_rate_until_smpc_changes_the_clock) {
    onchip_reset(0);
    CHECK_EQ(onchip_phi_clocks(1000000000ull), 26874125ull);
    smpc_write(0x1F, 0x0E);                     // CKCHG352
    CHECK_EQ(onchip_phi_clocks(1000000000ull), 28636363ull);
    smpc_write(0x1F, 0x0F);                     // CKCHG320
    CHECK_EQ(onchip_phi_clocks(1000000000ull), 26874125ull);
}

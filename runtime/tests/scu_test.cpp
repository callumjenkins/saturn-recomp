// The SCU's A-bus external interrupts, through IST as the program reads it.
#include "saturn.h"
#include "test.h"

namespace {
const uint32_t kIst = 0xA4, kAbusAck = 0xA8, kLine12 = 1u << 28;
}

TEST(an_external_line_is_pending_once_until_the_program_acknowledges_a_bus_interrupts) {
    scu_external(12, true);
    CHECK_EQ(scu_read(kIst, 4) & kLine12, kLine12);
    scu_write(kIst, ~kLine12, 4);                            // the handler clears it, the line still high
    scu_external(12, true);
    CHECK_EQ(scu_read(kIst, 4) & kLine12, 0);
    scu_write(kAbusAck, 1, 4);
    CHECK_EQ(scu_read(kIst, 4) & kLine12, kLine12);
    scu_write(kIst, ~kLine12, 4);
    scu_external(12, false);
    scu_write(kAbusAck, 1, 4);
    CHECK_EQ(scu_read(kIst, 4) & kLine12, 0);
}

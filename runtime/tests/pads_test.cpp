// The pads: which player each controller drives, and what INTBACK reports for each port.
#include "saturn.h"
#include "pad_slots.h"
#include "test.h"
#include <string>
#include <vector>

namespace {

// INTBACK's peripheral data for both ports, 15-byte mode
std::vector<uint8_t> peripherals() {
    smpc_write(0x01, 0x00);
    smpc_write(0x03, 0x08);
    smpc_write(0x1F, 0x10);
    std::vector<uint8_t> out;
    for (int i = 0; i < 32; ++i) out.push_back((uint8_t)smpc_read(0x21 + 2 * i));
    return out;
}

void press(const char* spec) {
    std::string error;
    CHECK_EQ(smpc_press(spec, error), true);
}

}  // namespace

TEST(controllers_take_the_lowest_free_player) {
    PadSlots s(12);
    CHECK_EQ(s.connect(10, "pad"), 0);
    CHECK_EQ(s.connect(11, "pad"), 1);
    CHECK_EQ(s.connect(12, "other"), 2);
}

TEST(a_controller_leaving_moves_nobody_else) {
    PadSlots s(12);
    s.connect(10, "pad");
    s.connect(11, "pad");
    s.connect(12, "pad");
    CHECK_EQ(s.disconnect(11), 1);
    CHECK_EQ(s.slot_of(10), 0);
    CHECK_EQ(s.slot_of(12), 2);
    CHECK_EQ(s.taken(1), false);
}

TEST(a_reconnecting_model_gets_its_slot_back_only_when_unambiguous) {
    PadSlots s(12);
    s.connect(10, "a");
    s.connect(11, "b");
    s.connect(12, "c");
    s.disconnect(12);
    CHECK_EQ(s.connect(13, "c"), 2);        // the one slot model c left, though 1 frees first below
    s.disconnect(10);
    s.disconnect(11);
    CHECK_EQ(s.connect(14, "b"), 1);
    s.disconnect(14);
    s.disconnect(13);
    PadSlots t(12);
    t.connect(20, "same");
    t.connect(21, "same");
    t.connect(22, "other");
    t.disconnect(22);
    t.disconnect(20);
    t.disconnect(21);
    CHECK_EQ(t.connect(23, "same"), 0);     // two slots left by that model: the lowest free
}

TEST(a_controller_beyond_the_last_player_drives_nothing) {
    PadSlots s(2);
    s.connect(10, "pad");
    s.connect(11, "pad");
    CHECK_EQ(s.connect(12, "pad"), -1);
    CHECK_EQ(s.disconnect(12), -1);
}

// These run in order: a script's pad stays plugged in once named.
TEST(port_2_is_empty_until_a_script_drives_pad_2) {
    g_cfg.multitap = 0;
    std::vector<uint8_t> d = peripherals();
    CHECK_EQ(d[0], 0xF1);                   // port 1: one pad
    CHECK_EQ(d[1], 0x02);
    CHECK_EQ(d[4], 0xF0);                   // port 2: nothing
    press("2.A");
    d = peripherals();
    CHECK_EQ(d[4], 0xF1);
    CHECK_EQ(d[5], 0x02);
    CHECK_EQ(d[6], 0xFB);                   // A held, active low
    CHECK_EQ(d[7], 0xFF);
    CHECK_EQ(d[2], 0xFF);                   // pad 1 untouched
}

TEST(with_one_multitap_port_2_holds_pad_7) {
    g_cfg.multitap = 1;
    press("7.B");
    press("6.C");
    std::vector<uint8_t> d = peripherals();
    CHECK_EQ(d[0], 0x16);                   // port 1: a 6-player multitap
    CHECK_EQ(d[1 + 5 * 3 + 1], 0xFD);       // its sixth pad: C held
    CHECK_EQ(d[19], 0xF1);                  // port 2: pad 7
    CHECK_EQ(d[21], 0xFE);                  // B held
    g_cfg.multitap = 0;
}

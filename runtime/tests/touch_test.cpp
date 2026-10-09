// The touchscreen's Saturn pad: where its buttons are and what fingers on it hold.
#include "touch_pad.h"
#include "test.h"
#include <vector>

namespace {

const TouchButton& button(const TouchLayout& l, uint16_t bit) {
    for (auto& b : l.buttons)
        if (b.bit == bit) return b;
    return l.buttons.front();
}

uint16_t held(const TouchLayout& l, std::vector<TouchFinger> fingers) { return touch_buttons(l, fingers); }

}  // namespace

TEST(each_button_is_pressed_on_its_centre_and_nothing_between_them) {
    for (auto [w, h] : {std::pair{2520, 1080}, std::pair{2184, 1968}, std::pair{1280, 800}}) {
        TouchLayout l = touch_layout(w, h);
        CHECK_EQ(l.buttons.size(), 9);
        for (auto& b : l.buttons) CHECK_EQ(held(l, {{b.x, b.y, false}}), b.bit);
        CHECK_EQ(held(l, {{w / 2.0f, h / 2.0f, false}}), 0);
    }
}

TEST(the_buttons_stay_on_the_screen_and_apart) {
    for (auto [w, h] : {std::pair{2520, 1080}, std::pair{2184, 1968}, std::pair{1280, 800}}) {
        TouchLayout l = touch_layout(w, h);
        for (auto& a : l.buttons) {
            float rw = a.shape == TouchButton::Round ? a.w : a.w / 2, rh = a.shape == TouchButton::Round ? a.w : a.h / 2;
            CHECK_EQ(a.x - rw >= 0 && a.x + rw <= w && a.y - rh >= 0 && a.y + rh <= h, true);
            for (auto& b : l.buttons)
                if (&a != &b) CHECK_EQ(touch_distance(a, b.x, b.y) > 0, true);
            CHECK_EQ(touch_on_dpad(l, a.x, a.y), false);
        }
    }
}

TEST(a_finger_between_a_and_b_presses_the_nearer) {
    TouchLayout l = touch_layout(2520, 1080);
    const TouchButton &a = button(l, B_A), &b = button(l, B_B);
    CHECK_EQ(held(l, {{a.x + 0.4f * (b.x - a.x), a.y, false}}), B_A);
    CHECK_EQ(held(l, {{a.x + 0.6f * (b.x - a.x), a.y, false}}), B_B);
}

TEST(the_dpad_gives_eight_directions_and_rests_in_the_middle) {
    TouchLayout l = touch_layout(2520, 1080);
    float x = l.dpad_x, y = l.dpad_y, r = l.dpad_r;
    CHECK_EQ(held(l, {{x, y, true}}), 0);
    CHECK_EQ(held(l, {{x + r, y, true}}), B_RIGHT);
    CHECK_EQ(held(l, {{x, y - r, true}}), B_UP);
    CHECK_EQ(held(l, {{x - r, y + r, true}}), B_LEFT | B_DOWN);
    CHECK_EQ(held(l, {{x + 3 * r, y + 0.2f * r, true}}), B_RIGHT);     // slid off, still the d-pad's
}

TEST(fingers_together_hold_all_they_press) {
    TouchLayout l = touch_layout(2520, 1080);
    const TouchButton &c = button(l, B_C), &start = button(l, B_START);
    CHECK_EQ(held(l, {{l.dpad_x, l.dpad_y - l.dpad_r, true}, {c.x, c.y, false}, {start.x, start.y, false}}),
             B_UP | B_C | B_START);
}

// saturn-recomp runtime — a Saturn pad drawn on a touchscreen (host.cpp draws it and feeds it fingers).
//
// The d-pad sits bottom left, A B C with X Y Z above them bottom right, START between them, L and R
// in the top corners, and the menu button top centre. Sizes follow the screen's shorter side, so the
// pad keeps its shape on a phone, a folded-out phone and a tablet.
#pragma once
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct TouchButton {
    enum Shape { Round, Pill, Shoulder };
    const char* label;
    uint16_t bit;
    Shape shape;
    float x, y;            // the centre, in pixels
    float w, h;            // Round: w is the radius; Pill and Shoulder: the size
};

struct TouchLayout {
    float dpad_x, dpad_y, dpad_r;           // the d-pad's centre and the reach of its arms
    float menu_x, menu_y, menu_r;
    std::vector<TouchButton> buttons;
};

// A finger on the screen. One that went down on the d-pad stays on it however far it slides.
struct TouchFinger {
    float x, y;
    bool on_dpad;
};

inline TouchLayout touch_layout(int w, int h) {
    float u = (float)std::min(w, h), m = 0.05f * u;
    TouchLayout l;
    l.dpad_r = 0.17f * u;
    l.dpad_x = m + l.dpad_r;
    l.dpad_y = h - m - l.dpad_r;
    l.menu_r = 0.045f * u;
    l.menu_x = w / 2.0f;
    l.menu_y = m + l.menu_r;
    // A B C a row, rising to the right as the pad's do; X Y Z smaller, above and a little to the right
    float r = 0.068f * u, gap = 2.45f * r, rise = 0.18f * r;
    float cx = w - m - r, cy = h - m - r - 2 * rise;
    const char* low[] = {"A", "B", "C"};
    const char* high[] = {"X", "Y", "Z"};
    const uint16_t low_bits[] = {B_A, B_B, B_C}, high_bits[] = {B_X, B_Y, B_Z};
    for (int i = 0; i < 3; ++i) {
        float x = cx - (2 - i) * gap, y = cy + (2 - i) * rise;
        l.buttons.push_back({low[i], low_bits[i], TouchButton::Round, x, y, r, r});
        l.buttons.push_back({high[i], high_bits[i], TouchButton::Round, x + 0.45f * r, y - 2.25f * r, 0.72f * r, 0.72f * r});
    }
    l.buttons.push_back({"START", B_START, TouchButton::Pill, w / 2.0f, h - m - 0.35f * r, 2.6f * r, 0.9f * r});
    float sw = 0.3f * u, sh = 0.1f * u;
    l.buttons.push_back({"L", B_L, TouchButton::Shoulder, m + sw / 2, m + sh / 2, sw, sh});
    l.buttons.push_back({"R", B_R, TouchButton::Shoulder, w - m - sw / 2, m + sh / 2, sw, sh});
    return l;
}

inline bool touch_on_dpad(const TouchLayout& l, float x, float y) {
    return std::hypot(x - l.dpad_x, y - l.dpad_y) < 1.2f * l.dpad_r;
}

inline bool touch_on_menu(const TouchLayout& l, float x, float y) {
    return std::hypot(x - l.menu_x, y - l.menu_y) < 1.5f * l.menu_r;
}

// How far a finger is outside the button, in pixels: 0 or less is on it.
inline float touch_distance(const TouchButton& b, float x, float y) {
    if (b.shape == TouchButton::Round) return std::hypot(x - b.x, y - b.y) - b.w;
    float dx = std::max(std::fabs(x - b.x) - b.w / 2, 0.0f), dy = std::max(std::fabs(y - b.y) - b.h / 2, 0.0f);
    return std::hypot(dx, dy);
}

// The buttons the fingers hold. A finger on the d-pad holds the one or two directions it leans
// towards; any other presses the nearest button within half a button's size of it.
inline uint16_t touch_buttons(const TouchLayout& l, const std::vector<TouchFinger>& fingers) {
    uint16_t held = 0;
    for (auto& f : fingers) {
        if (f.on_dpad) {
            float dx = f.x - l.dpad_x, dy = f.y - l.dpad_y, d = std::hypot(dx, dy);
            if (d < 0.2f * l.dpad_r) continue;
            const float lean = 0.38f;                    // sin 22.5°: eight directions of 45° each
            if (dx / d > lean) held |= B_RIGHT;
            if (dx / d < -lean) held |= B_LEFT;
            if (dy / d > lean) held |= B_DOWN;
            if (dy / d < -lean) held |= B_UP;
            continue;
        }
        const TouchButton* best = nullptr;
        float best_d = 0;
        for (auto& b : l.buttons) {
            float d = touch_distance(b, f.x, f.y), slack = 0.5f * std::min(b.w, b.h);
            if (d < slack && (!best || d < best_d)) best = &b, best_d = d;
        }
        if (best) held |= best->bit;
    }
    return held;
}

// saturn-recomp runtime — a gamepad made by Linux's uinput, for --virtual-pads uinput (host.cpp).
//
// The kernel makes it an input device as it does a USB pad, with an Xbox 360 pad's buttons and axes,
// and SDL finds it through udev and reads it through evdev, as it does a real pad. A write is in the
// device's queue when it returns, so SDL's next event pump reads it.
#pragma once
#include <cstdint>
#include <string>

struct UinputPad {
    int fd = -1;
    std::string path;           // its /dev/input/eventN, as SDL_GetGamepadPath gives it
};

struct UinputState {
    bool south, east, west, north, left_shoulder, right_shoulder, start;
    bool left_trigger, right_trigger;
    int hat_x, hat_y;           // the d-pad: -1, 0 or 1
    int stick_x, stick_y;       // the left stick, pushed all the way: -1, 0 or 1
};

// A pad with this USB vendor and product; on failure, a fd of -1 and the reason in error.
UinputPad uinput_pad_open(uint16_t vendor, uint16_t product, std::string& error);
void uinput_pad_set(const UinputPad& pad, const UinputState& s);
void uinput_pad_close(UinputPad& pad);         // unplugs it

// saturn-recomp runtime — a gamepad made by Linux's uinput (uinput_pad.h).
#include "uinput_pad.h"

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace {

// the buttons xpad gives a 360 pad, which SDL numbers by key code; uinput_pad_set's list follows this order
const int kKeys[] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};

struct Axis { int code, min, max, fuzz, flat; };
const Axis kAxes[] = {{ABS_X, -32768, 32767, 16, 128}, {ABS_Y, -32768, 32767, 16, 128},
                      {ABS_RX, -32768, 32767, 16, 128}, {ABS_RY, -32768, 32767, 16, 128},
                      {ABS_Z, 0, 255, 0, 0}, {ABS_RZ, 0, 255, 0, 0},
                      {ABS_HAT0X, -1, 1, 0, 0}, {ABS_HAT0Y, -1, 1, 0, 0}};

UinputPad fail(int fd, const char* what, std::string& error) {
    error = std::string(what) + ": " + std::strerror(errno);
    if (fd >= 0) close(fd);
    return {};
}

// The evdev node the kernel made for the device: /sys/devices/virtual/input/inputN/eventM
std::string event_node(int fd) {
    char sys[64] = {};
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sys - 1), sys) < 0) return {};
    std::string dir = std::string("/sys/devices/virtual/input/") + sys;
    std::string node;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (!std::strncmp(e->d_name, "event", 5)) node = std::string("/dev/input/") + e->d_name;
        closedir(d);
    }
    return node;
}

void emit(int fd, int type, int code, int value) {
    input_event e = {};
    e.type = (uint16_t)type;
    e.code = (uint16_t)code;
    e.value = value;
    ssize_t n = write(fd, &e, sizeof e);      // a lost event shows as a frame that differs
    (void)n;
}

}  // namespace

UinputPad uinput_pad_open(uint16_t vendor, uint16_t product, std::string& error) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return fail(-1, "/dev/uinput", error);
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
        return fail(fd, "uinput", error);
    for (int k : kKeys)
        if (ioctl(fd, UI_SET_KEYBIT, k) < 0) return fail(fd, "uinput", error);
    for (const Axis& a : kAxes) {
        uinput_abs_setup s = {};
        s.code = (uint16_t)a.code;
        s.absinfo.minimum = a.min;
        s.absinfo.maximum = a.max;
        s.absinfo.fuzz = a.fuzz;
        s.absinfo.flat = a.flat;
        if (ioctl(fd, UI_SET_ABSBIT, a.code) < 0 || ioctl(fd, UI_ABS_SETUP, &s) < 0) return fail(fd, "uinput", error);
    }
    uinput_setup u = {};
    u.id = {BUS_USB, vendor, product, 0x0110};
    std::strncpy(u.name, "saturn-recomp uinput pad", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd, UI_DEV_SETUP, &u) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) return fail(fd, "uinput", error);
    UinputPad pad{fd, event_node(fd)};
    if (pad.path.empty()) {
        error = "uinput: no event device for the pad";
        uinput_pad_close(pad);
    }
    return pad;
}

void uinput_pad_set(const UinputPad& pad, const UinputState& s) {
    const bool down[] = {s.south, s.east, s.west, s.north, s.left_shoulder, s.right_shoulder, false, s.start};
    for (size_t i = 0; i < sizeof down / sizeof *down; ++i) emit(pad.fd, EV_KEY, kKeys[i], down[i]);
    emit(pad.fd, EV_ABS, ABS_HAT0X, s.hat_x);
    emit(pad.fd, EV_ABS, ABS_HAT0Y, s.hat_y);
    emit(pad.fd, EV_ABS, ABS_X, s.stick_x * 32767);
    emit(pad.fd, EV_ABS, ABS_Y, s.stick_y * 32767);
    emit(pad.fd, EV_ABS, ABS_Z, s.left_trigger ? 255 : 0);
    emit(pad.fd, EV_ABS, ABS_RZ, s.right_trigger ? 255 : 0);
    emit(pad.fd, EV_SYN, SYN_REPORT, 0);
}

void uinput_pad_close(UinputPad& pad) {
    if (pad.fd < 0) return;
    ioctl(pad.fd, UI_DEV_DESTROY);
    close(pad.fd);
    pad.fd = -1;
}

#else

UinputPad uinput_pad_open(uint16_t, uint16_t, std::string& error) {
    error = "uinput pads need Linux";
    return {};
}
void uinput_pad_set(const UinputPad&, const UinputState&) {}
void uinput_pad_close(UinputPad&) {}

#endif

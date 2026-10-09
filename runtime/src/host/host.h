// saturn-recomp runtime — what the machine asks of the host (host.cpp): a
// window for the picture, the pad, the pace and an audio stream. The rest of
// the runtime reaches SDL only through these.
#pragma once
#include <atomic>
#include <cstdint>
#include "video.h"

bool host_open();                                    // the window (unless headless); false if it failed
void host_present(const Frame& f);                   // show it, take the events
void host_pace(uint64_t now);                        // wait until the host's clock has caught up
bool host_wants_frame();                             // a window to show it in
const int kHostSlots = 12;                           // players, numbered across both ports (smpc.cpp)
enum : uint16_t {                                    // a Saturn pad's buttons, as host_pad gives them
    B_RIGHT = 0x8000, B_LEFT = 0x4000, B_DOWN = 0x2000, B_UP = 0x1000, B_START = 0x0800, B_A = 0x0400,
    B_C = 0x0200, B_B = 0x0100, B_R = 0x0080, B_X = 0x0040, B_Y = 0x0020, B_Z = 0x0010, B_L = 0x0008,
};
uint16_t host_pad(int slot);                         // buttons held on that player's controllers, smpc.cpp's bits
bool host_pad_connected(int slot);                   // a controller drives it: the keyboard always drives the first

bool host_audio_open();                              // with a window; false: no device
void host_audio_push(const int16_t* lr, int frames); // interleaved stereo, 44 100 Hz
void host_audio_report();                            // how the stream fared, to stderr

// the host call the machine's thread is in, or "" between them, for the stall report (machine.cpp)
extern std::atomic<const char*> g_host_call;
extern std::atomic<int> g_host_paused;              // above 0: the menu is open or the app is in the background, so no VBlank is no stall

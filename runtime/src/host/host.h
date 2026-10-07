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
uint16_t host_pad();                                 // buttons held, smpc.cpp's bits

bool host_audio_open();                              // with a window; false: no device
void host_audio_push(const int16_t* lr, int frames); // interleaved stereo, 44 100 Hz
void host_audio_report();                            // how the stream fared, to stderr

// the host call the machine's thread is in, or "" between them, for the stall report (machine.cpp)
extern std::atomic<const char*> g_host_call;

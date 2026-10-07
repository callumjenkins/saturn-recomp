// saturn-recomp runtime — the host for a build without SDL: headless runs
// only, with no window, pad or sound device. Pictures still go to PNG files.
#include "saturn.h"
#include "host.h"

bool host_open() {
    if (g_cfg.headless) return true;
    sat_note("built without SDL: no window (run with --headless)");
    return false;
}
void host_present(const Frame&) {}
void host_pace(uint64_t) {}
bool host_wants_frame() { return false; }
uint16_t host_pad(int) { return 0; }
bool host_pad_connected(int) { return false; }

bool host_audio_open() { return false; }
void host_audio_push(const int16_t*, int) {}
void host_audio_report() {}

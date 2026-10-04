// saturnkit runtime — boots a disc into the recompiled programs of this
// build and runs it, in a window or headless.
//
//     saturn --cue GAME.cue [--out DIR] [--headless] [--fullscreen] [--scale N]
//            [--vblanks N] [--starts N] [--trace] [--realtime] [--input VBLANK:BUTTONS,...|@FILE]
//            [--record-input FILE] [--shot N,...] [--dump N,...] [--peek ADDR[:WORDS],...] [--watch LO:HI]
//            [--watch-vblanks FROM:TO] [--wav FILE] [--interp] [--hook ADDR:rN=VALUE ...]
//            [--tasks SETJMP:LONGJMP] [--multitap N] [--clock YYYY-MM-DDTHH:MM:SS]
//
// --vblanks and --starts end the run after that many VBlanks or program
// starts; the log of the hardware touched goes to DIR/hw-log.txt. --shot
// saves the picture at those VBlank-INs (DIR/shot-N.png), --dump the video
// memories (DIR/dump-N.bin), --wav the sound of the whole run. --hook sets
// register N to VALUE (hex) after the hooked instruction at ADDR (the build's
// recomp --hook). --watch LO:HI logs each access to a memory area in the
// range with --trace; in the work RAMs, each store, with the function that
// made it, with or without --trace; --watch-vblanks FROM:TO narrows that to
// those VBlanks. --record-input writes the pad as the host presses it (the
// keyboard, a gamepad), sampled once a VBlank, as an --input script: given
// back with --input @FILE, the run goes the same way again. --interp draws the fields between the game's frames with
// everything moved part of the way (vdp1.cpp), one frame behind. --tasks names
// the game's setjmp and longjmp when it switches its own tasks with them
// (tasks.cpp); the build hooks both addresses. --multitap puts 6-player
// multitaps on port 1, or (2) on both ports (smpc.cpp); --input then takes
// "N." before a pad's buttons. --clock sets the SMPC's clock at power-on,
// which then runs with the run's time; without it the clock is the host's.
#include "saturn.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// YYYY-MM-DDTHH:MM:SS as seconds since 1970, no time zone; -1 if it does not parse.
static int64_t clock_seconds(const char* s) {
    int y, mo, d, h, mi, se;
    if (std::sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6 || y < 1970 || mo < 1 || mo > 12) return -1;
    y -= mo <= 2;                                // days from civil (H. Hinnant's algorithm)
    int64_t era = y / 400, yoe = y - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

int main(int argc, char** argv) {
    SaturnConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        bool more = i + 1 < argc;
        if (!std::strcmp(a, "--cue") && more) cfg.cue = argv[++i];
        else if (!std::strcmp(a, "--out") && more) cfg.out = argv[++i];
        else if (!std::strcmp(a, "--vblanks") && more) cfg.stop_vblanks = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(a, "--starts") && more) cfg.stop_starts = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--trace")) cfg.trace = true;
        else if (!std::strcmp(a, "--peek") && more) cfg.peek = argv[++i];
        else if (!std::strcmp(a, "--watch") && more) {
            const char* w = argv[++i];
            cfg.watch_lo = (uint32_t)std::strtoul(w, nullptr, 16);
            const char* colon = std::strchr(w, ':');
            cfg.watch_hi = colon ? (uint32_t)std::strtoul(colon + 1, nullptr, 16) : cfg.watch_lo;
        }
        else if (!std::strcmp(a, "--watch-vblanks") && more) {
            const char* w = argv[++i];
            cfg.watch_from = std::strtoull(w, nullptr, 10);
            const char* colon = std::strchr(w, ':');
            cfg.watch_to = colon ? std::strtoull(colon + 1, nullptr, 10) : ~0ull;
        }
        else if (!std::strcmp(a, "--realtime")) cfg.realtime = true;
        else if (!std::strcmp(a, "--input") && more) cfg.input = argv[++i];
        else if (!std::strcmp(a, "--record-input") && more) cfg.record_input = argv[++i];
        else if (!std::strcmp(a, "--dump") && more) cfg.dump = argv[++i];
        else if (!std::strcmp(a, "--shot") && more) cfg.shots = argv[++i];
        else if (!std::strcmp(a, "--headless")) cfg.headless = true;
        else if (!std::strcmp(a, "--fullscreen")) cfg.fullscreen = true;
        else if (!std::strcmp(a, "--scale") && more) cfg.scale = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--wav") && more) cfg.wav = argv[++i];
        else if (!std::strcmp(a, "--interp")) cfg.interp = true;
        else if (!std::strcmp(a, "--multitap") && more) cfg.multitap = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--clock") && more) {
            cfg.clock = clock_seconds(argv[++i]);
            if (cfg.clock < 0) { std::fprintf(stderr, "--clock: YYYY-MM-DDTHH:MM:SS\n"); return 2; }
        }
        else if (!std::strcmp(a, "--tasks") && more) {
            if (std::sscanf(argv[++i], "%x:%x", &cfg.task_setjmp, &cfg.task_longjmp) != 2) {
                std::fprintf(stderr, "saturn: --tasks SETJMP:LONGJMP, not %s\n", argv[i]);
                return 2;
            }
        }
        else if (!std::strcmp(a, "--hook") && more) {
            unsigned addr, reg, v;
            if (std::sscanf(argv[++i], "%x:r%u=%x", &addr, &reg, &v) != 3 || reg > 15) {
                std::fprintf(stderr, "saturn: --hook ADDR:rN=VALUE, not %s\n", argv[i]);
                return 2;
            }
            sh2_hook_set(addr, (int)reg, v);
        }
        else {
            std::fprintf(stderr, "usage: saturn --cue GAME.cue [--out DIR] [--headless] [--fullscreen] [--scale N]\n"
                                 "             [--vblanks N] [--starts N] [--trace] [--realtime] [--input VBLANK:BUTTONS,...|@FILE]\n"
                                 "             [--record-input FILE] [--shot N,...] [--dump N,...] [--peek ADDR[:WORDS],...] [--watch LO:HI]\n"
                                 "             [--wav FILE] [--hook ADDR:rN=VALUE ...] [--tasks SETJMP:LONGJMP] [--multitap N]\n"
                                 "             [--clock YYYY-MM-DDTHH:MM:SS]\n");
            return 2;
        }
    }
    if (cfg.cue.empty()) { std::fprintf(stderr, "saturn: --cue is required\n"); return 2; }
    return saturn_main(cfg);
}

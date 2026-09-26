// saturnkit runtime — boots a disc into the recompiled programs of this
// build and runs it, in a window or headless.
//
//     saturn --cue GAME.cue [--out DIR] [--headless] [--fullscreen] [--scale N]
//            [--vblanks N] [--starts N] [--trace] [--realtime] [--input VBLANK:BUTTONS,...]
//            [--shot N,...] [--dump N,...] [--peek ADDR[:WORDS],...] [--watch LO:HI]
//            [--watch-vblanks FROM:TO] [--wav FILE] [--interp] [--hook ADDR:rN=VALUE ...]
//
// --vblanks and --starts end the run after that many VBlanks or program
// starts; the log of the hardware touched goes to DIR/hw-log.txt. --shot
// saves the picture at those VBlank-INs (DIR/shot-N.png), --dump the video
// memories (DIR/dump-N.bin), --wav the sound of the whole run. --hook sets
// register N to VALUE (hex) after the hooked instruction at ADDR (the build's
// recomp --hook). --watch LO:HI logs each access to a memory area in the
// range with --trace; in the work RAMs, each store, with the function that
// made it, with or without --trace; --watch-vblanks FROM:TO narrows that to
// those VBlanks. --interp draws the fields between the game's frames with
// everything moved part of the way (vdp1.cpp), one frame behind.
#include "saturn.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
        else if (!std::strcmp(a, "--dump") && more) cfg.dump = argv[++i];
        else if (!std::strcmp(a, "--shot") && more) cfg.shots = argv[++i];
        else if (!std::strcmp(a, "--headless")) cfg.headless = true;
        else if (!std::strcmp(a, "--fullscreen")) cfg.fullscreen = true;
        else if (!std::strcmp(a, "--scale") && more) cfg.scale = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--wav") && more) cfg.wav = argv[++i];
        else if (!std::strcmp(a, "--interp")) cfg.interp = true;
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
                                 "             [--vblanks N] [--starts N] [--trace] [--realtime] [--input VBLANK:BUTTONS,...]\n"
                                 "             [--shot N,...] [--dump N,...] [--peek ADDR[:WORDS],...] [--watch LO:HI]\n"
                                 "             [--wav FILE] [--hook ADDR:rN=VALUE ...]\n");
            return 2;
        }
    }
    if (cfg.cue.empty()) { std::fprintf(stderr, "saturn: --cue is required\n"); return 2; }
    return saturn_main(cfg);
}

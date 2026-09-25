// saturnkit runtime — a Saturn with no screen yet: boots a disc into the
// recompiled programs of this build and runs it.
//
//     saturn --cue GAME.cue [--out DIR] [--vblanks N] [--starts N] [--trace] [--realtime]
//            [--peek ADDR[:WORDS],...] [--watch LO:HI] [--input VBLANK:BUTTONS,...]
//
// --vblanks and --starts end the run after that many VBlanks or program
// starts; the log of the hardware touched goes to DIR/hw-log.txt.
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
        else if (!std::strcmp(a, "--realtime")) cfg.realtime = true;
        else if (!std::strcmp(a, "--input") && more) cfg.input = argv[++i];
        else {
            std::fprintf(stderr, "usage: saturn --cue GAME.cue [--out DIR] [--vblanks N] [--starts N] [--trace] [--realtime]\n"
                                 "             [--peek ADDR[:WORDS],...] [--watch LO:HI] [--input VBLANK:BUTTONS,...]\n");
            return 2;
        }
    }
    if (cfg.cue.empty()) { std::fprintf(stderr, "saturn: --cue is required\n"); return 2; }
    return saturn_main(cfg);
}

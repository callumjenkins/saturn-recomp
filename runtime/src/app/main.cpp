// saturn-recomp runtime — boots a disc into the recompiled programs of this
// build and runs it, in a window or headless.
//
//     saturn --cue GAME.cue [--out DIR] [--headless] [--fullscreen] [--scale N]
//            [--vblanks N] [--starts N] [--trace] [--realtime] [--input VBLANK:BUTTONS,...|@FILE]
//            [--record-input FILE] [--shot N,...] [--dump N,...] [--peek ADDR[:WORDS],...] [--watch LO:HI]
//            [--watch-vblanks FROM:TO] [--wav FILE] [--interp] [--hook ADDR:rN=VALUE ...]
//            [--tasks SETJMP:LONGJMP] [--multitap N] [--clock YYYY-MM-DDTHH:MM:SS] [--agent FD]
//            [--write VBLANK:ADDR=HEX,...] [--video FILE] [--save FILE|-] [--coverage FILE] [--checkpoint SECONDS]
//            [--log FILE] [--launcher]
//
// --vblanks and --starts end the run after that many VBlanks or program
// starts; the log of the hardware touched goes to DIR/hw-log.txt. --shot
// saves the picture at those VBlank-INs (DIR/shot-N.png), --dump the video
// memories (DIR/dump-N.bin), --wav the sound of the whole run. --hook sets
// register N to VALUE (hex) after the hooked instruction at ADDR (the build's
// recomp --hook). --watch LO:HI logs each access to a memory area in the
// range with --trace; in the work RAMs, each store, with the function that
// made it, with or without --trace; --watch-vblanks FROM:TO narrows that to
// those VBlanks. --record-input writes the pads as the host presses them (the
// keyboard and gamepads, a player each), sampled once a VBlank, as an --input
// script: given back with --input @FILE, the run goes the same way again, and
// takes nothing from the host's controllers. --virtual-input SCRIPT, for tests, presses the
// script's pads on SDL virtual gamepads instead, a player each, through the window's input. --interp draws the fields between the game's frames with
// everything moved part of the way (vdp1.cpp), one frame behind. --tasks names
// the game's setjmp and longjmp when it switches its own tasks with them
// (tasks.cpp); the build hooks both addresses. --multitap puts 6-player
// multitaps on port 1, or (2) on both ports (smpc.cpp). --input takes "N."
// before a pad's buttons for player N, numbered across the ports. --clock sets the SMPC's clock at power-on,
// which then runs with the run's time; without it the clock is the host's.
// --agent hands the run to another program through the socket at FD
// (agent.cpp): it steps VBlanks, presses the pads, reads memory and frames.
// --coverage writes every recompiled function, with its module, address, instruction count and
// whether it ran, when the run ends, an error that ends it included; --checkpoint SECONDS writes it again every
// SECONDS of the host's time. --video writes the run's pictures and sound as an MP4 through ffmpeg, an
// error that ends it included (movie.cpp).
// --write sets memory at a VBlank-IN, as the agent's write does: a stage
// select, say, given as data.
// --save names the file the game's saves (the backup memory) are kept in; "-"
// keeps them for the run alone, starting empty. Without it they go to
// saturn-recomp/PRODUCT_VERSION/backup.bin in the user's data directory
// ($XDG_DATA_HOME or ~/.local/share, ~/Library/Application Support, %APPDATA%).
// --settings names the player's settings file, which a run with a window reads (the window, the
// volume, the controls; host.cpp): "-" keeps the defaults. Without it, it is settings.ini in that
// directory's saturn-recomp folder, written with the defaults if it is missing.
// --launcher opens a menu in the window before the run, for the disc, the players, the controls,
// the display and the volume (launcher.cpp); it opens too when there is no --cue and a window. What it
// chooses for the run, as saturn's own arguments, goes to DIR/launch.txt
// (with --out), one a line, for a replay.
// --log writes what would go to stdout and stderr to FILE instead, for a host that shows neither (Android).
#include "saturn.h"
#include "host.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__ANDROID__)
#include <SDL3/SDL_main.h>                   // SDL's Java side starts the run through SDL_main
#endif

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
    bool launcher = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        bool more = i + 1 < argc;
        if (!std::strcmp(a, "--cue") && more) cfg.cue = argv[++i];
        else if (!std::strcmp(a, "--out") && more) cfg.out = argv[++i];
        else if (!std::strcmp(a, "--save") && more) cfg.save = argv[++i];
        else if (!std::strcmp(a, "--vblanks") && more) cfg.stop_vblanks = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(a, "--starts") && more) cfg.stop_starts = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--trace")) cfg.trace = true;
        else if (!std::strcmp(a, "--peek") && more) cfg.peek = argv[++i];
        else if (!std::strcmp(a, "--coverage") && more) cfg.coverage = argv[++i];
        else if (!std::strcmp(a, "--checkpoint") && more) cfg.checkpoint = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--log") && more) {
            const char* log = argv[++i];
            if (!std::freopen(log, "a", stdout) || !std::freopen(log, "a", stderr)) return 2;
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);       // a stream reopened on a file may come back buffered (bionic's does)
        }
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
        else if (!std::strcmp(a, "--virtual-input") && more) cfg.virtual_input = argv[++i];
        else if (!std::strcmp(a, "--dump") && more) cfg.dump = argv[++i];
        else if (!std::strcmp(a, "--shot") && more) cfg.shots = argv[++i];
        else if (!std::strcmp(a, "--headless")) cfg.headless = true;
        else if (!std::strcmp(a, "--launcher")) launcher = true;
        else if (!std::strcmp(a, "--fullscreen")) cfg.fullscreen = true;
        else if (!std::strcmp(a, "--scale") && more) cfg.scale = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--settings") && more) cfg.settings = argv[++i];
        else if (!std::strcmp(a, "--wav") && more) cfg.wav = argv[++i];
        else if (!std::strcmp(a, "--video") && more) cfg.video = argv[++i];
        else if (!std::strcmp(a, "--interp")) cfg.interp = true;
        else if (!std::strcmp(a, "--multitap") && more) cfg.multitap = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--agent") && more) cfg.agent_fd = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--write") && more) cfg.writes += (cfg.writes.empty() ? "" : ",") + std::string(argv[++i]);
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
            std::fprintf(stderr, "usage: saturn --cue GAME.cue [--out DIR] [--headless] [--fullscreen] [--scale N] [--settings FILE]\n"
                                 "             [--vblanks N] [--starts N] [--trace] [--realtime] [--input VBLANK:BUTTONS,...|@FILE]\n"
                                 "             [--record-input FILE] [--shot N,...] [--dump N,...] [--peek ADDR[:WORDS],...] [--watch LO:HI]\n"
                                 "             [--wav FILE] [--hook ADDR:rN=VALUE ...] [--tasks SETJMP:LONGJMP] [--multitap N]\n"
                                 "             [--clock YYYY-MM-DDTHH:MM:SS] [--agent FD] [--write VBLANK:ADDR=HEX,...] [--video FILE]\n"
                                 "             [--save FILE|-] [--coverage FILE] [--checkpoint SECONDS] [--log FILE] [--launcher]\n");
            return 2;
        }
    }
    if (launcher || (cfg.cue.empty() && !cfg.headless)) {
        if (!host_launch(cfg)) return host_wants_frame() ? 0 : 2;       // closed, or no window to show it in
        if (FILE* f = cfg.out == "." ? nullptr : std::fopen((cfg.out + "/launch.txt").c_str(), "w")) {
            std::fprintf(f, "--multitap\n%d\n", cfg.multitap);
            std::fclose(f);
        }
    }
    if (cfg.cue.empty()) { std::fprintf(stderr, "saturn: --cue is required\n"); return 2; }
    return saturn_main(cfg);
}

// saturn-recomp runtime — the recompiler's self-test: recompiled functions run
// on the vectors src/saturnrecomp/recomp/selftest.py recorded with the interpreter
// (sh2emu), and every result compared.
//
//     selftest VECTORS.txt ...
//
// A vectors file is lines of:
//     image BASE PATH        load a program image at BASE (hex)
//     module NAME            activate that module (its image must be in memory)
//     func ENTRY LABEL       the function the next vectors call
//     v IN... OUT...         22 hex words each: r0-r15, sr, gbr, vbr, mach, macl, pr
//     mem CRC_L CRC_H        crc32 of WRAM-L and WRAM-H after the function's vectors
// Memory is shared by all vectors in order, as in the interpreter's run.
#include "saturn/sh2.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static const uint32_t kSentinel = 0xFFFFFFF0u;     // sh2emu.RETURN_SENTINEL
static const char* kNames[22] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
                                 "r11", "r12", "r13", "r14", "r15", "sr", "gbr", "vbr", "mach", "macl", "pr"};

// The stub services end the run on a hardware access or a bad call: say
// which vector it was.
static std::string g_where;
static void where() {
    if (!g_where.empty()) std::fprintf(stderr, "  during %s\n", g_where.c_str());
}

static void put(SH2Context& c, const uint32_t* w) {
    for (int i = 0; i < 16; ++i) c.r[i] = w[i];
    sh2_set_sr(c, w[16]);
    c.gbr = w[17]; c.vbr = w[18]; c.mach = w[19]; c.macl = w[20]; c.pr = w[21];
}

static void get(const SH2Context& c, uint32_t* w) {
    for (int i = 0; i < 16; ++i) w[i] = c.r[i];
    w[16] = sh2_get_sr(c);
    w[17] = c.gbr; w[18] = c.vbr; w[19] = c.mach; w[20] = c.macl; w[21] = c.pr;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: selftest VECTORS.txt ...\n");
        return 2;
    }
    std::atexit(where);
    long total_funcs = 0, total_vec = 0, total_bad = 0;
    for (int fi = 1; fi < argc; ++fi) {
        std::ifstream in(argv[fi]);
        if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[fi]); return 2; }
        std::string line, label;
        uint32_t entry = 0;
        long funcs = 0, vec = 0, bad = 0, fbad = 0, fvec = 0;
        std::memset(g_wram_l, 0, 0x100000);
        std::memset(g_wram_h, 0, 0x100000);
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string kw;
            ss >> kw;
            if (kw.empty() || kw[0] == '#') continue;
            if (kw == "image") {
                std::string base, path;
                ss >> base >> std::ws;
                std::getline(ss, path);
                std::ifstream img(path, std::ios::binary);
                std::vector<char> data((std::istreambuf_iterator<char>(img)), std::istreambuf_iterator<char>());
                if (data.empty() || !sh2_mem_write((uint32_t)std::stoul(base, nullptr, 16), data.data(), data.size())) {
                    std::fprintf(stderr, "cannot load %s at %s\n", path.c_str(), base.c_str());
                    return 2;
                }
            } else if (kw == "module") {
                std::string name;
                ss >> name;
                const SH2Module* m = sh2_module(name.c_str());
                if (!m) { std::fprintf(stderr, "no module %s in this build\n", name.c_str()); return 2; }
                if (sh2_identify(m->base) != m) {
                    std::fprintf(stderr, "the image in memory at %08X is not module %s\n", m->base, name.c_str());
                    return 2;
                }
                sh2_activate(m);
            } else if (kw == "func") {
                std::string e;
                ss >> e >> std::ws;
                std::getline(ss, label);
                entry = (uint32_t)std::stoul(e, nullptr, 16);
                ++funcs; fbad = 0; fvec = 0;
            } else if (kw == "v") {
                uint32_t w[44], got[22];
                for (auto& x : w) { std::string h; ss >> h; x = (uint32_t)std::stoul(h, nullptr, 16); }
                SH2Context c{};
                put(c, w);
                c.pr = kSentinel; c.pc = 0; c.budget = 1 << 20;
                g_where = std::string(argv[fi]) + ": " + label + ", vector " + std::to_string(fvec + 1);
                sh2_call(c, entry);
                g_where.clear();
                get(c, got);
                ++vec; ++fvec;
                bool ok = c.pc == kSentinel && !std::memcmp(got, w + 22, sizeof got);
                if (!ok) {
                    ++bad;
                    if (fbad++ < 3) {
                        std::printf("FAIL %08X %s, vector %ld:", entry, label.c_str(), fvec);
                        if (c.pc != kSentinel) std::printf(" returned to %08X", c.pc);
                        for (int i = 0; i < 22; ++i)
                            if (got[i] != w[22 + i]) std::printf(" %s=%08X (want %08X)", kNames[i], got[i], w[22 + i]);
                        std::printf("\n      in:");
                        for (int i = 0; i < 22; ++i) std::printf(" %08X", w[i]);
                        std::printf("\n");
                    }
                }
            } else if (kw == "mem") {
                std::string l, h;
                ss >> l >> h;
                uint32_t cl = sh2_crc32(g_wram_l, 0x100000), ch = sh2_crc32(g_wram_h, 0x100000);
                if (cl != (uint32_t)std::stoul(l, nullptr, 16) || ch != (uint32_t)std::stoul(h, nullptr, 16)) {
                    ++bad;
                    std::printf("FAIL %08X %s: memory differs after its vectors (WRAM-L %s, WRAM-H %s)\n",
                                entry, label.c_str(), cl == std::stoul(l, nullptr, 16) ? "same" : "differs",
                                ch == std::stoul(h, nullptr, 16) ? "same" : "differs");
                    // go on from the interpreter's state is not possible: stop this file
                    break;
                }
            }
        }
        std::printf("%s: %ld functions, %ld vectors, %ld failures\n", argv[fi], funcs, vec, bad);
        total_funcs += funcs; total_vec += vec; total_bad += bad;
    }
    if (argc > 2)
        std::printf("total: %ld functions, %ld vectors, %ld failures\n", total_funcs, total_vec, total_bad);
    return total_bad ? 1 : 0;
}

// saturnkit runtime — the SMPC: its commands, INTBACK and the pads.
//
// Registers are bytes at odd addresses (IREG0-6 0x01-0x0D, COMREG 0x1F,
// OREG0-31 0x21-0x5F, SR 0x61, SF 0x63, the ports 0x75-0x7F). A command
// written to COMREG completes at once: OREG31 holds its code and SF drops.
// INTBACK answers with the status (time, area, system state) and/or the
// peripheral data, and raises the SMPC interrupt at the next poll; the
// peripheral data comes when the program asks to continue (IREG0 bit 7).
// Port 1 has a standard digital pad, pressed by a script (--input) and by
// the host's keyboard and gamepad (host.cpp), port 2 nothing. The pad can
// also be read directly (IOSEL set, the SH-2 driving TH and TR through
// PDR1 and DDR1): each TH/TR pair selects four of its lines, active low,
//   TH TR   D3    D2    D1    D0
//    0  0   R     X     Y     Z
//    0  1   RIGHT LEFT  DOWN  UP
//    1  0   START A     C     B
//    1  1   L     1     0     0     (with TL 1: the pad's ID, 0xB)
// the same bits INTBACK reports in its two bytes (byte 1: 01 then 10, byte 2:
// 00 then 11), and the order MAME's Saturn driver reads them in.
#include "saturn.h"
#include "video.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static uint8_t g_ireg[7], g_oreg[32], g_sr, g_sf, g_port[16];
static uint8_t g_smem[4];
static bool g_irq, g_peri_pending;
static uint8_t g_area = 0x0C;              // the disc's area (smpc_set_area); TV timing is NTSC (video.cpp)
static uint16_t g_script_pressed;         // what the script holds now

// The pad, scripted: "VBLANK:BUTTON+BUTTON,..." (an empty list releases all).
// Digital pad bits, active low: byte 1 RIGHT LEFT DOWN UP START A C B,
// byte 2 R X Y Z L.
static std::vector<std::pair<uint64_t, uint16_t>> g_script;
static size_t g_script_pos;

static const struct { const char* name; uint16_t bit; } kButtons[] = {
    {"RIGHT", 0x8000}, {"LEFT", 0x4000}, {"DOWN", 0x2000}, {"UP", 0x1000}, {"START", 0x0800},
    {"A", 0x0400}, {"C", 0x0200}, {"B", 0x0100}, {"R", 0x0080}, {"X", 0x0040}, {"Y", 0x0020},
    {"Z", 0x0010}, {"L", 0x0008}};

static uint16_t buttons(const std::string& list) {
    uint16_t pressed = 0;
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find('+', i);
        std::string b = list.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? list.size() : j + 1;
        bool known = false;
        for (auto& k : kButtons)
            if (b == k.name) { pressed |= k.bit; known = true; }
        if (!known && !b.empty()) sat_fatal("--input: no button %s", b.c_str());
    }
    return pressed;
}

void smpc_input_script(const std::string& given) {
    std::string spec = given;
    if (!spec.empty() && spec[0] == '@') {               // @FILE: the script in a file (--record-input's)
        std::ifstream f(spec.substr(1));
        if (!f) sat_fatal("--input: cannot read %s", spec.c_str() + 1);
        std::stringstream ss;
        ss << f.rdbuf();
        spec = ss.str();
        while (!spec.empty() && (spec.back() == '\n' || spec.back() == '\r' || spec.back() == ',')) spec.pop_back();
    }
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        std::string item = spec.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? spec.size() : j + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) sat_fatal("--input: %s is not VBLANK:BUTTONS", item.c_str());
        g_script.push_back({std::stoull(item.substr(0, colon)), buttons(item.substr(colon + 1))});
    }
}

static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

static void intback_status() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    int year = tm->tm_year + 1900;
    g_oreg[0] = 0x80;                        // STE: the clock is set; RESD 0
    g_oreg[1] = bcd(year / 100);
    g_oreg[2] = bcd(year % 100);
    g_oreg[3] = (uint8_t)(tm->tm_wday << 4 | (tm->tm_mon + 1));
    g_oreg[4] = bcd(tm->tm_mday);
    g_oreg[5] = bcd(tm->tm_hour);
    g_oreg[6] = bcd(tm->tm_min);
    g_oreg[7] = bcd(tm->tm_sec);
    g_oreg[8] = 0;                           // cartridge code
    g_oreg[9] = g_area;
    g_oreg[10] = 0x34 | 0x02;                // system status 1: bits as an emulator sets them (not checked)
    g_oreg[11] = 0;                          // system status 2 (CDRES 0)
    for (int i = 0; i < 4; ++i) g_oreg[12 + i] = g_smem[i];
    g_oreg[31] = 0x10;
}

void smpc_set_area(char symbol) {
    static const char kSymbols[] = "JTUBKAEL";
    static const uint8_t kCodes[] = {0x1, 0x2, 0x4, 0x5, 0x6, 0xA, 0xC, 0xD};
    for (int i = 0; i < 8; ++i)
        if (symbol == kSymbols[i]) g_area = kCodes[i];
}

// The host's pad (keyboard, gamepad), taken once a VBlank, as a script's is,
// so that --record-input writes what the program saw and --input @FILE
// gives it back at the same VBlanks
static uint64_t g_host_vblank = ~0ull;
static uint16_t g_host_pressed;
static FILE* g_record;

static void record_host(uint64_t vblank, uint16_t pressed) {
    if (g_cfg.record_input.empty()) return;
    if (!g_record && !(g_record = std::fopen(g_cfg.record_input.c_str(), "w")))
        sat_fatal("--record-input: cannot write %s", g_cfg.record_input.c_str());
    std::string names;
    for (auto& k : kButtons)
        if (pressed & k.bit) names += (names.empty() ? "" : "+") + std::string(k.name);
    std::fprintf(g_record, "%llu:%s,", (unsigned long long)vblank, names.c_str());
    std::fflush(g_record);
}

// port 1's pad now, active low: byte 1 RIGHT LEFT DOWN UP START A C B, byte 2 R X Y Z L 1 1 1
static uint16_t pad1_now() {
    while (g_script_pos < g_script.size() && g_script[g_script_pos].first <= sat_vblanks()) {
        g_script_pressed = g_script[g_script_pos].second;
        sat_note("pad: %04X", g_script_pressed);
        ++g_script_pos;
    }
    if (sat_vblanks() != g_host_vblank) {
        g_host_vblank = sat_vblanks();
        uint16_t h = host_pad();
        if (h != g_host_pressed) record_host(g_host_vblank, h);
        g_host_pressed = h;
    }
    return (uint16_t)~(g_script_pressed | g_host_pressed) | 0x0007;
}

// PDR1 read in direct mode: the lines the SH-2 drives (DDR1) read back as written,
// TH and TR pulled up when not driven, TL high, D3-D0 from the pad
static uint8_t pdr1_direct() {
    uint8_t out = g_port[0x5], ddr = g_port[0x9] & 0x7F;
    uint8_t lines = (uint8_t)((out & ddr) | (0x60 & ~ddr));
    bool th = lines & 0x40, tr = lines & 0x20;
    uint16_t pad = pad1_now();
    uint8_t nib = th ? (tr ? (uint8_t)((pad & 0x8) | 0x4) : (uint8_t)(pad >> 8 & 0xF))
                     : (tr ? (uint8_t)(pad >> 12) : (uint8_t)(pad >> 4 & 0xF));
    return (uint8_t)(0x80 | (lines & 0x60) | 0x10 | (nib & 0xF));
}

static void intback_peripheral() {
    uint16_t pad1 = pad1_now();
    int i = 0;
    g_oreg[i++] = 0xF1;                      // port 1: direct, one peripheral
    g_oreg[i++] = 0x02;                      // digital pad, 2 bytes
    g_oreg[i++] = (uint8_t)(pad1 >> 8);
    g_oreg[i++] = (uint8_t)pad1;
    g_oreg[i++] = 0xF0;                      // port 2: nothing
    while (i < 31) g_oreg[i++] = 0;
    g_oreg[31] = 0x10;
    g_sr = 0xC0 | (g_ireg[1] >> 4 & 0x0F);   // peripheral data, the last of it (PDE 0)
}

static void command(uint8_t cmd) {
    switch (cmd) {
    case 0x00: sat_trace("SMPC MSHON"); break;
    case 0x02: slave_on(); break;
    case 0x03: slave_off(); break;
    case 0x06: sat_trace("SMPC SNDON"); sound_power(true); break;
    case 0x07: sat_trace("SMPC SNDOFF"); sound_power(false); break;
    case 0x08: sat_trace("SMPC CDON"); break;
    case 0x09: sat_trace("SMPC CDOFF"); break;
    case 0x0D: sat_stop("SMPC SYSRES: the program reset the system");
    case 0x0E: case 0x0F: sat_trace("SMPC CKCHG%s", cmd == 0x0E ? "352" : "320"); break;
    case 0x10: {                             // INTBACK
        bool status = g_ireg[0] & 1, peri = g_ireg[1] & 8;
        if (status) {
            intback_status();
            g_sr = 0x40 | (peri ? 0x20 : 0) | (g_ireg[1] >> 4 & 0x0F);
            g_peri_pending = peri;
        } else if (peri) {
            intback_peripheral();
            g_peri_pending = false;
        }
        g_irq = true;
        break;
    }
    case 0x16: sat_trace("SMPC SETTIME"); break;
    case 0x17: for (int i = 0; i < 4; ++i) g_smem[i] = g_ireg[i]; break;
    case 0x18: sat_trace("SMPC NMIREQ"); break;
    case 0x19: case 0x1A: break;             // RESENAB, RESDISA
    default: sat_fatal("SMPC command %02X", cmd);
    }
    g_oreg[31] = cmd;
    g_sf = 0;
}

uint32_t smpc_read(uint32_t off) {
    if (off >= 0x21 && off <= 0x5F) return g_oreg[(off - 0x21) / 2];
    if (off >= 0x01 && off <= 0x0D) return g_ireg[(off - 1) / 2];
    switch (off) {
    case 0x61: return g_sr;
    case 0x63: return g_sf;
    case 0x75: return (g_port[0xD] & 1) ? pdr1_direct() : 0xFF;   // PDR1: direct mode when IOSEL1 is set
    case 0x77: return 0xFF;                  // PDR2: nothing pulled low
    }
    if (off >= 0x70) return g_port[off & 0xF];
    return 0;
}

void smpc_write(uint32_t off, uint32_t v) {
    if (off >= 0x01 && off <= 0x0D) {
        g_ireg[(off - 1) / 2] = (uint8_t)v;
        if (off == 0x01 && g_peri_pending && (v & 0x80)) {   // continue: the peripheral data
            intback_peripheral();
            g_peri_pending = false;
            g_irq = true;
        } else if (off == 0x01 && g_peri_pending && (v & 0x40)) {   // break
            g_peri_pending = false;
            g_sr &= ~0x20;
        }
        return;
    }
    switch (off) {
    case 0x1F: command((uint8_t)v); return;
    case 0x63: g_sf = (uint8_t)v; return;
    }
    if (off >= 0x70) g_port[off & 0xF] = (uint8_t)v;
}

void smpc_tick() {
    if (g_irq) {
        g_irq = false;
        scu_raise(IRQ_SMPC);
    }
}

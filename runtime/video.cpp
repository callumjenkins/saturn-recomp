// saturnkit runtime — the raster timing that drives the frame, the video and
// sound chips' place on the bus, and the sound driver's side of its
// handshake.
//
// Timing is NTSC: 263 lines at 59.94 Hz, whatever the disc's area (TVSTAT's
// PAL bit reads 0). Each line reached runs the SCU's timer 0 compare and
// raises HBlank-IN; line 0 raises VBlank-OUT, the first line after the
// display (224, or 240 with TVMD's VRESO 1; 224 too for VRESO 2, PAL's 256
// lines, which a 60 Hz raster has no room for) VBlank-IN. At VBlank-IN the
// field that has ended is composed (vdp2.cpp) when a window or a picture
// file wants it; at VBlank-OUT VDP1 erases and changes frame (vdp1.cpp).
//
// VDP1 is vdp1.cpp, VDP2's picture vdp2.cpp; VDP2's registers, VRAM and
// colour RAM, and the SCSP's RAM and registers, keep what is written.
#include "saturn.h"
#include "video.h"
#include <cstdio>
#include <string>
#include <vector>

static std::vector<uint8_t> g_scsp_ram(0x80000), g_scsp_regs(0x1000);

static const uint64_t kFrameNs = 16683350;      // 59.94 Hz
static const int kLines = 263;
static uint64_t g_line_abs;                      // raster lines since power-on
static uint64_t g_vblanks;
static Frame g_frame;

uint64_t sat_vblanks() { return g_vblanks; }
uint64_t video_frame_changes() { return vdp1_frame_changes(); }
uint64_t video_draws() { return vdp1_draws(); }

static uint16_t reg16(const uint8_t* r, uint32_t o) { return (uint16_t)(r[o] << 8 | r[o + 1]); }
static void set16(uint8_t* r, uint32_t o, uint16_t v) { r[o] = v >> 8; r[o + 1] = (uint8_t)v; }

void video_init() {
    vdp1_init();
    if (!host_open()) sat_fatal("no window");
}

static int display_lines() {
    int vreso = reg16(g_vdp2_regs, 0x00) >> 4 & 3;
    return vreso == 1 ? 240 : 224;
}

static bool listed(const std::string& list, const std::string& item) {
    return !list.empty() && ("," + list + ",").find("," + item + ",") != std::string::npos;
}

// --dump: VDP1 VRAM, its draw framebuffer, VDP2 VRAM, CRAM, VDP1's and VDP2's registers, one after the other
static void dump(const char* prefix, uint64_t n) {
    char path[512];
    std::snprintf(path, sizeof path, "%s/dump-%s%llu.bin", g_cfg.out.c_str(), prefix, (unsigned long long)n);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    vdp1_dump(f);
    std::fwrite(g_vdp2_vram, 1, sizeof g_vdp2_vram, f);
    std::fwrite(g_vdp2_cram, 1, sizeof g_vdp2_cram, f);
    std::fclose(f);
}

static void vblank_in(uint64_t now) {
    ++g_vblanks;
    std::string n = std::to_string(g_vblanks);
    if (listed(g_cfg.dump, n)) dump("", g_vblanks);
    bool shot = listed(g_cfg.shots, n);
    if (shot || host_wants_frame()) {
        vdp2_compose(g_frame);
        if (shot) write_png(g_cfg.out + "/shot-" + n + ".png", g_frame);
        host_present(g_frame);
    }
    scu_raise(IRQ_VBLANK_IN);
    scu_frame_event(0);
    host_pace(now);
}

void video_tick(uint64_t now) {
    uint64_t target = now / (kFrameNs / kLines);
    while (g_line_abs < target) {
        ++g_line_abs;
        int line = (int)(g_line_abs % kLines);
        if (line == 0) { vdp1_vblank_out(); scu_raise(IRQ_VBLANK_OUT); scu_frame_event(1); }
        if (line == display_lines()) vblank_in(g_line_abs * (kFrameNs / kLines));
        scu_raise(IRQ_HBLANK_IN);
        scu_line(line);
    }
    vdp1_tick(now);
}

// ---- the sound CPU's side, high level --------------------------------------------------------
// The 68000 is not run. What the SH-2 waits for from it is SBL's sound driver
// (SDDRVS) taking commands: the host writes 16-byte command blocks into
// sound RAM at 0x700 (8 of them) and waits until the driver has cleared a
// block's first byte. With the 68000 on, the driver takes every command at
// the next poll. The commands are logged, not performed.
//
// One command is followed further, PCM streaming (0x85 start, 0x86 stop):
// the driver plays a ring buffer of samples from sound RAM and publishes,
// for each of 8 streams, where it plays in the byte at 0x7A0 + 2 * stream:
// the play position in blocks of 4096 samples (the SCSP's call address,
// CA). The SH-2 counts each change of that byte as 4096 samples played
// (Virtual Hydlide's PCM task, 0x06055C36 and 0x060560C8), refills the ring
// by it and paces movies by it; published a sample at a time, the byte
// changed at every poll of the task and the movie ran 2.4 times too fast. Here the position runs from the start command's pitch word
// (SCSP OCT/FNS: 44.1 kHz * 2^OCT * (1 + FNS/1024)). The start command's
// layout, as Virtual Hydlide's movie player sends it: [1] stream, [2] mode
// (0x80 stereo), [3] level/pan, [4-5] buffer address >> 4, [6-7] size in
// samples (0: 65536), [8-9] pitch word.
static bool g_sound_on;
static uint64_t g_sound_cmds;
struct PcmStream { bool on; uint64_t t0; double rate; uint32_t size; };
static PcmStream g_pcm[8];

void sound_power(bool on) { g_sound_on = on; }

static void sound_command(const uint8_t* p) {
    ++g_sound_cmds;
    sat_trace("SND cmd %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", p[0], p[1], p[2], p[3], p[4], p[5],
              p[6], p[7], p[8], p[9], p[10], p[11]);
    if (p[0] == 0x85) {
        PcmStream& s = g_pcm[p[1] & 7];
        uint16_t pitch = (uint16_t)(p[8] << 8 | p[9]);
        int oct = (pitch >> 11) & 0xF;
        if (oct & 8) oct -= 16;
        s.rate = 44100.0 * (oct >= 0 ? (double)(1 << oct) : 1.0 / (1 << -oct)) * (1.0 + (pitch & 0x3FF) / 1024.0);
        s.size = (uint32_t)(p[6] << 8 | p[7]);
        if (!s.size) s.size = 0x10000;
        s.t0 = sat_now();
        s.on = true;
        sat_trace("SND: PCM stream %d starts, %.0f Hz, %u samples at %05X", p[1] & 7, s.rate, s.size, (p[4] << 8 | p[5]) << 4);
    } else if (p[0] == 0x86) {
        g_pcm[p[1] & 7].on = false;
    }
}

void sound_tick() {
    if (!g_sound_on) return;
    for (uint32_t b = 0x700; b < 0x780; b += 0x10) {
        uint8_t* p = &g_scsp_ram[b];
        if (!p[0]) continue;
        sound_command(p);
        p[0] = 0;
    }
    uint64_t now = sat_now();
    for (int i = 0; i < 8; ++i)
        if (g_pcm[i].on) {
            uint32_t pos = (uint32_t)((double)(now - g_pcm[i].t0) * g_pcm[i].rate / 1e9) % g_pcm[i].size;
            g_scsp_ram[0x7A0 + 2 * i] = (uint8_t)(pos >> 12);
        }
}

// ---- the bus -------------------------------------------------------------------------
bool video_owns(uint32_t a) {
    return (a >= 0x05A00000u && a < 0x05B01000u) || (a >= 0x05C00000u && a < 0x05D00020u) ||
           (a >= 0x05E00000u && a < 0x05F80200u);
}

static uint8_t* area(uint32_t a, uint32_t& off) {
    if (a < 0x05B00000u) { off = a & 0x7FFFF; return g_scsp_ram.data(); }
    if (a < 0x05B01000u) { off = a & 0xFFF; return g_scsp_regs.data(); }
    if (a < 0x05C80000u) { off = a & 0x7FFFF; return g_vdp1_vram; }
    if (a < 0x05E00000u) return nullptr;                  // VDP1's framebuffer and registers
    if (a < 0x05F00000u) { off = a & 0x7FFFF; return g_vdp2_vram; }
    if (a < 0x05F80000u) { off = a & 0xFFF; return g_vdp2_cram; }
    return nullptr;
}

uint32_t video_read(uint32_t a, int size) {
    uint32_t off;
    if (uint8_t* p = area(a, off)) return mem_rd(p, off, size);
    if (a < 0x05D00000u) return vdp1_fb_read(a & 0x3FFFF, size);
    if (a < 0x05D00020u) return vdp1_reg_read(a & 0x1F, size);
    off = a & 0x1FF;
    if (off == 0x04 || off == 0x08 || off == 0x0A) {   // TVSTAT, HCNT, VCNT: from the raster
        int line = (int)(g_line_abs % kLines);
        uint16_t tvstat = (uint16_t)((line >= display_lines() ? 8 : 0) | ((g_vblanks & 1) ? 2 : 0));
        set16(g_vdp2_regs, 0x04, tvstat);
        set16(g_vdp2_regs, 0x0A, (uint16_t)line);
    }
    return mem_rd(g_vdp2_regs, off, size);
}

void video_write(uint32_t a, uint32_t v, int size) {
    uint32_t off;
    if (uint8_t* p = area(a, off)) { mem_wr(p, off, v, size); return; }
    if (a < 0x05D00000u) { vdp1_fb_write(a & 0x3FFFF, v, size); return; }
    if (a < 0x05D00020u) { vdp1_reg_write(a & 0x1F, v, size); return; }
    mem_wr(g_vdp2_regs, a & 0x1FF, v, size);
}

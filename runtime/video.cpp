// saturnkit runtime — VDP1, VDP2 and the SCSP, as memory for now, and the
// raster timing that drives the frame.
//
// Timing is NTSC: 263 lines at 59.94 Hz, whatever the disc's area (TVSTAT's
// PAL bit reads 0). Each line reached runs the SCU's timer 0 compare and
// raises HBlank-IN; line 0 raises VBlank-OUT, the first line after the
// display (224 or 240, from TVMD) VBlank-IN. At VBlank-IN VDP1 changes frame
// (every frame in one-cycle mode, when FBCR asked for it in manual mode) and,
// with PTMR = 2, starts drawing; PTMR = 1 starts at once. Nothing is drawn
// yet: the command list is walked (for the counts and the end address), the
// end flags in EDSR are set a little later and the sprite-draw-end interrupt
// raised.
//
// All memory and all registers keep what is written; reads return it.
#include "saturn.h"
#include <vector>

static std::vector<uint8_t> g_vdp1_vram(0x80000), g_vdp1_fb(0x40000), g_vdp2_vram(0x80000),
    g_vdp2_cram(0x1000), g_scsp_ram(0x80000), g_scsp_regs(0x1000);
static uint8_t g_vdp1_regs[0x20], g_vdp2_regs[0x200];

static const uint64_t kFrameNs = 16683350;      // 59.94 Hz
static const int kLines = 263;
static uint64_t g_line_abs;                      // raster lines since power-on
static uint64_t g_vblanks, g_frame_changes, g_draws;
static bool g_drawing, g_change_pending;
static uint64_t g_draw_end;                      // time the current draw ends

uint64_t sat_vblanks() { return g_vblanks; }
uint64_t video_frame_changes() { return g_frame_changes; }
uint64_t video_draws() { return g_draws; }

static uint16_t reg16(const uint8_t* r, uint32_t o) { return (uint16_t)(r[o] << 8 | r[o + 1]); }
static void set16(uint8_t* r, uint32_t o, uint16_t v) { r[o] = v >> 8; r[o + 1] = (uint8_t)v; }

void video_init() {
    set16(g_vdp1_regs, 0x16, 0x1000);            // MODR: version 1
    set16(g_vdp1_regs, 0x10, 0x0002);            // EDSR: the last draw has ended
}

static int display_lines() {
    int vreso = reg16(g_vdp2_regs, 0x00) >> 4 & 3;
    return vreso == 1 ? 240 : 224;
}

// ---- VDP1 ---------------------------------------------------------------------------
static void draw_start() {
    uint16_t edsr = reg16(g_vdp1_regs, 0x10);
    set16(g_vdp1_regs, 0x10, (uint16_t)(edsr >> 1 & 1));   // BEF <- CEF, CEF <- 0
    // walk the command table: END, and the jump modes (next, assign, call, return, skips)
    uint32_t a = 0, ret = 0, last = 0;
    int n = 0;
    for (int guard = 0; guard < 20000; ++guard) {
        uint16_t ctrl = (uint16_t)mem_rd(g_vdp1_vram.data(), a, 2);
        last = a;
        if (ctrl & 0x8000) break;
        int jp = ctrl >> 12 & 7;
        if (!(jp & 4)) ++n;                      // not a skip
        uint32_t link = (uint32_t)mem_rd(g_vdp1_vram.data(), a + 2, 2) * 8 & 0x7FFFF;
        switch (jp & 3) {
        case 0: a += 0x20; break;
        case 1: a = link; break;
        case 2: ret = a + 0x20; a = link; break;
        case 3: a = ret; break;
        }
        a &= 0x7FFFF;
    }
    set16(g_vdp1_regs, 0x12, (uint16_t)(last >> 3));       // LOPR
    set16(g_vdp1_regs, 0x14, (uint16_t)(last >> 3));       // COPR
    ++g_draws;
    g_drawing = true;
    g_draw_end = sat_now() + 1000000;           // 1 ms
    sat_trace("VDP1 draw %llu: %d commands", (unsigned long long)g_draws, n);
}

static void frame_change() {
    ++g_frame_changes;
    sat_trace("VDP1 frame change %llu", (unsigned long long)g_frame_changes);
    if ((reg16(g_vdp1_regs, 0x04) & 3) == 2) draw_start();   // PTMR: draw at each frame change
}

static void vblank_in() {
    ++g_vblanks;
    scu_raise(IRQ_VBLANK_IN);
    scu_frame_event(0);
    uint16_t fbcr = reg16(g_vdp1_regs, 0x02);
    bool manual = fbcr & 2;                      // FCM
    if (!manual || g_change_pending) {
        g_change_pending = false;
        frame_change();
    }
}

void video_tick(uint64_t now) {
    uint64_t target = now / (kFrameNs / kLines);
    while (g_line_abs < target) {
        ++g_line_abs;
        int line = (int)(g_line_abs % kLines);
        if (line == 0) { scu_raise(IRQ_VBLANK_OUT); scu_frame_event(1); }
        if (line == display_lines()) vblank_in();
        scu_raise(IRQ_HBLANK_IN);
        scu_line(line);
    }
    if (g_drawing && now >= g_draw_end) {
        g_drawing = false;
        set16(g_vdp1_regs, 0x10, (uint16_t)(reg16(g_vdp1_regs, 0x10) | 2));   // CEF
        scu_raise(IRQ_SPRITE_END);
        scu_frame_event(6);
    }
}

static void vdp1_reg_write(uint32_t off, uint16_t v) {
    set16(g_vdp1_regs, off, v);
    switch (off) {
    case 0x02:                                   // FBCR: FCM|FCT asks for a change at the next VBlank
        if ((v & 3) == 3) g_change_pending = true;
        break;
    case 0x04:                                   // PTMR
        if ((v & 3) == 1) draw_start();
        break;
    }
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
// for each of 8 streams, the play position at 0x7A0 + 2 * stream (samples
// from the buffer's start, 16 bits); the SH-2 refills the ring and paces
// movies by it. Here the position runs from the start command's pitch word
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
            set16(&g_scsp_ram[0x7A0 + 2 * i], 0, (uint16_t)pos);
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
    if (a < 0x05C80000u) { off = a & 0x7FFFF; return g_vdp1_vram.data(); }
    if (a < 0x05D00000u) { off = a & 0x3FFFF; return g_vdp1_fb.data(); }
    if (a < 0x05E00000u) return nullptr;                  // VDP1's registers
    if (a < 0x05F00000u) { off = a & 0x7FFFF; return g_vdp2_vram.data(); }
    if (a < 0x05F80000u) { off = a & 0xFFF; return g_vdp2_cram.data(); }
    return nullptr;
}

uint32_t video_read(uint32_t a, int size) {
    uint32_t off;
    if (uint8_t* p = area(a, off)) return mem_rd(p, off, size);
    if (a >= 0x05D00000u && a < 0x05D00020u) return mem_rd(g_vdp1_regs, a & 0x1F, size);
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
    if (a >= 0x05D00000u && a < 0x05D00020u) {
        off = a & 0x1F;
        if (size == 4) { vdp1_reg_write(off, (uint16_t)(v >> 16)); vdp1_reg_write(off + 2, (uint16_t)v); }
        else if (size == 2) vdp1_reg_write(off, (uint16_t)v);
        else sat_fatal("byte write to VDP1 register %02X", off);
        return;
    }
    mem_wr(g_vdp2_regs, a & 0x1FF, v, size);
}

// saturn-recomp runtime — VDP2: the picture of a field, composed in software
// from VRAM, colour RAM, the registers and VDP1's framebuffer.
//
// What is done: the four normal scroll screens (NBG0-NBG3) in cell mode
// (1x1 or 2x2 cells, 1- or 2-word pattern names with the supplement, plane
// sizes, map offsets, 16/256/2048 colours and RGB) and NBG0/NBG1 in bitmap
// mode; NBG0/NBG1's fractional scroll and coordinate increments (zoom);
// the sprite layer from VDP1's framebuffer, every sprite type, palette and
// RGB mixed (SPCLMD), with its priorities and colour-calculation ratios;
// priorities with the chip's order for ties (sprite, NBG0, NBG1, NBG2,
// NBG3); special priority and special colour calculation by screen or by
// character, and colour calculation by the colour's MSB; colour calculation
// of the top two layers (ratio of the top one, or add); windows 0 and 1, as
// rectangles or line windows, on the scroll screens, the sprite layer and
// colour calculation; the colour offsets A and B; the back screen (one
// colour or one a line); the display bit; the high resolutions (640 and 704
// dots), with double-density interlace composed as one picture of both
// fields' lines (448, 480 or 512), the scroll screens a dot row a line;
// shadows (normal, MSB and transparent) on the scroll screens, the back
// screen and the sprite itself.
// What is not: RBG0 and RBG1 (rotation), line and vertical-cell scroll,
// mosaic, the sprite window, the line colour screen, special priority and
// colour calculation by dot (the special function codes), gradation,
// extended colour calculation, the exclusive resolutions. A register that asks for one of them is noted once.
#include "saturn.h"
#include "video.h"
#include <algorithm>
#include <cstring>

uint8_t g_vdp2_vram[0x80000], g_vdp2_cram[0x1000], g_vdp2_regs[0x200];

static uint16_t reg(uint32_t off) { return (uint16_t)(g_vdp2_regs[off] << 8 | g_vdp2_regs[off + 1]); }
static uint8_t vb(uint32_t a) { return g_vdp2_vram[a & 0x7FFFF]; }
static uint16_t vw(uint32_t a) { a &= 0x7FFFE; return (uint16_t)(g_vdp2_vram[a] << 8 | g_vdp2_vram[a + 1]); }

int vdp2_lines() {
    switch (reg(0x00) >> 4 & 3) {
    case 1: return 240;
    case 2: return 256;
    default: return 224;
    }
}

static int cram_mode() { return reg(0x0E) >> 12 & 3; }

// A colour RAM entry as 0x00RRGGBB, and its MSB (the colour calculation bit).
static uint32_t cram(uint32_t i, bool* msb) {
    if (cram_mode() == 2) {
        uint32_t a = (i & 0x3FF) * 4;
        uint32_t v = (uint32_t)g_vdp2_cram[a] << 24 | g_vdp2_cram[a + 1] << 16 | g_vdp2_cram[a + 2] << 8 | g_vdp2_cram[a + 3];
        if (msb) *msb = v >> 31;
        return (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF);
    }
    uint32_t a = (i & (cram_mode() == 1 ? 0x7FF : 0x3FF)) * 2;
    uint16_t v = (uint16_t)(g_vdp2_cram[a] << 8 | g_vdp2_cram[a + 1]);
    if (msb) *msb = v >> 15;
    return (uint32_t)((v & 0x1F) << 3) << 16 | ((v >> 5 & 0x1F) << 3) << 8 | ((v >> 10 & 0x1F) << 3);
}

static uint32_t rgb555(uint16_t v) {
    return (uint32_t)((v & 0x1F) << 3) << 16 | ((v >> 5 & 0x1F) << 3) << 8 | ((v >> 10 & 0x1F) << 3);
}

// One layer's pixel on a line.
struct Pix {
    uint32_t rgb;
    uint8_t prio;                               // 0: nothing here
    uint8_t ratio;                              // colour-calculation ratio, 0-31
    bool cc;                                    // colour calculation on
    bool offset;                                // colour offset on
    bool offset_b;                              // ... offset B rather than A
    bool spr, scc;                              // the character's special priority and colour calculation bits
    bool msb;                                   // the colour data's MSB
    bool shade;                                 // an MSB shadow: the sprite dot at half brightness
};

enum { L_SPRITE, L_NBG0, L_NBG1, L_NBG2, L_NBG3, L_COUNT };

// ---- the scroll screens ---------------------------------------------------------------------
struct Nbg {
    bool on, opaque, bitmap, cell2x2, word1, cnsm;
    int colors;                                 // 0 16, 1 256, 2 2048, 3 32K RGB, 4 16M RGB
    uint16_t supp;                              // the pattern name supplement
    int plane_w, plane_h;                       // in pages
    uint32_t plane[4];                          // A B C D: VRAM addresses
    uint32_t page_bytes;
    int bm_w, bm_h;                             // bitmap size
    uint32_t bm_addr;
    uint16_t bm_pal;
    bool bm_spr, bm_scc;
    uint32_t caos;                              // colour RAM offset, in entries
    int32_t sx, sy, dx, dy;                     // scroll and increments, 8 fraction bits
    uint8_t prio, ratio;
    bool cc, offset, offset_b;
};

static void setup_nbg(int n, Nbg& s) {
    s = Nbg{};
    uint16_t bgon = reg(0x20);
    s.on = bgon >> n & 1;
    s.opaque = bgon >> (8 + n) & 1;
    if (!s.on) return;
    uint16_t chctl = n < 2 ? reg(0x28) : reg(0x2A);
    int sh = (n & 1) * 8;
    if (n < 2) {
        uint16_t c = (uint16_t)(chctl >> sh);
        s.colors = n == 0 ? c >> 4 & 7 : c >> 4 & 3;
        s.bitmap = c >> 1 & 1;
        s.cell2x2 = c & 1;
        int bmsz = c >> 2 & 3;
        s.bm_w = bmsz & 2 ? 1024 : 512;
        s.bm_h = bmsz & 1 ? 512 : 256;
    } else {
        uint16_t c = (uint16_t)(chctl >> ((n - 2) * 4));
        s.colors = c >> 1 & 1;
        s.cell2x2 = c & 1;
    }
    uint16_t pncn = reg(0x30 + n * 2);
    s.word1 = pncn >> 15 & 1;
    s.cnsm = pncn >> 14 & 1;
    s.supp = pncn & 0x3FF;
    int plsz = reg(0x3A) >> (n * 2) & 3;
    s.plane_w = plsz == 0 ? 1 : 2;
    s.plane_h = plsz == 3 ? 2 : 1;
    uint32_t mpof = reg(0x3C) >> (n * 4) & 7;
    s.page_bytes = (s.cell2x2 ? 0x800u : 0x2000u) * (s.word1 ? 1 : 2);
    uint32_t mask = (uint32_t)(s.plane_w * s.plane_h - 1);
    for (int i = 0; i < 4; ++i) {
        uint16_t mp = reg(0x40 + n * 4 + (i / 2) * 2);
        uint32_t m = (mpof << 6 | (uint32_t)(mp >> ((i & 1) * 8) & 0x3F)) & ~mask;
        s.plane[i] = m * s.page_bytes;
    }
    if (s.bitmap) {
        s.bm_addr = mpof * 0x20000;
        uint16_t bmpn = (uint16_t)(reg(0x2C) >> sh);
        s.bm_pal = (uint16_t)((bmpn & 7) << 4);
        s.bm_spr = bmpn >> 5 & 1;
        s.bm_scc = bmpn >> 4 & 1;
    }
    int caos_sh = n * 4;
    s.caos = (uint32_t)(reg(0xE4) >> caos_sh & 7) << 8;
    if (n < 2) {
        uint32_t b = 0x70 + n * 0x10;
        s.sx = (int32_t)((reg(b) & 0x7FF) << 8 | reg(b + 2) >> 8);
        s.sy = (int32_t)((reg(b + 4) & 0x7FF) << 8 | reg(b + 6) >> 8);
        s.dx = (int32_t)((reg(b + 8) & 7) << 8 | reg(b + 10) >> 8);
        s.dy = (int32_t)((reg(b + 12) & 7) << 8 | reg(b + 14) >> 8);
    } else {
        uint32_t b = 0x90 + (n - 2) * 4;
        s.sx = (int32_t)(reg(b) & 0x7FF) << 8;
        s.sy = (int32_t)(reg(b + 2) & 0x7FF) << 8;
        s.dx = s.dy = 0x100;
    }
    uint16_t prin = reg(0xF8 + (n / 2) * 2);
    s.prio = (uint8_t)(prin >> ((n & 1) * 8) & 7);
    uint16_t ccrn = reg(0x108 + (n / 2) * 2);
    s.ratio = (uint8_t)(ccrn >> ((n & 1) * 8) & 0x1F);
    s.cc = reg(0xEC) >> n & 1;
    s.offset = reg(0x110) >> n & 1;
    s.offset_b = reg(0x112) >> n & 1;
}

// the dot of a character (or bitmap) at (x, y) in its own pixels: false if transparent
static bool dot(const Nbg& s, uint32_t base, int x, int y, int width, uint32_t pal, Pix& p) {
    uint32_t i = (uint32_t)(y * width + x);
    uint32_t v;
    switch (s.colors) {
    case 0: v = vb(base + i / 2); v = (i & 1) ? (v & 0xF) : (v >> 4); break;
    case 1: v = vb(base + i); break;
    case 2: v = vw(base + i * 2) & 0x7FF; break;
    case 3: {
        uint16_t w = vw(base + i * 2);
        if (!(w & 0x8000) && !s.opaque) return false;
        p.rgb = rgb555(w);
        p.msb = w >> 15;
        return true;
    }
    default: {
        uint32_t w = (uint32_t)vw(base + i * 4) << 16 | vw(base + i * 4 + 2);
        if (!(w & 0x80000000u) && !s.opaque) return false;
        p.rgb = (w & 0xFF) << 16 | (w & 0xFF00) | (w >> 16 & 0xFF);
        p.msb = w >> 31;
        return true;
    }
    }
    if (v == 0 && !s.opaque) return false;
    uint32_t index = s.colors == 0 ? pal * 16 + v : s.colors == 1 ? (pal & 0x70) * 16 + v : v;
    p.rgb = cram(index + s.caos, &p.msb);
    return true;
}

static bool nbg_pixel(const Nbg& s, int x, int y, Pix& p) {
    uint32_t sx = (uint32_t)(s.sx + x * s.dx) >> 8, sy = (uint32_t)(s.sy + y * s.dy) >> 8;
    if (s.bitmap) {
        sx &= (uint32_t)s.bm_w - 1;
        sy &= (uint32_t)s.bm_h - 1;
        p.spr = s.bm_spr;
        p.scc = s.bm_scc;
        return dot(s, s.bm_addr, (int)sx, (int)sy, s.bm_w, s.bm_pal, p);
    }
    uint32_t pw = (uint32_t)s.plane_w * 512, ph = (uint32_t)s.plane_h * 512;
    sx &= pw * 2 - 1;                            // the map: 2x2 planes
    sy &= ph * 2 - 1;
    int pl = (sy >= ph ? 2 : 0) + (sx >= pw ? 1 : 0);
    uint32_t ix = sx % pw, iy = sy % ph;
    uint32_t page = (iy / 512) * (uint32_t)s.plane_w + ix / 512;
    int cs = s.cell2x2 ? 16 : 8;
    uint32_t cx = (ix % 512) / (uint32_t)cs, cy = (iy % 512) / (uint32_t)cs;
    uint32_t cpr = 512 / (uint32_t)cs;
    uint32_t pnd_addr = s.plane[pl] + page * s.page_bytes + (cy * cpr + cx) * (s.word1 ? 2 : 4);
    uint32_t chr, pal;
    bool hf, vf;
    if (s.word1) {
        uint16_t w = vw(pnd_addr);
        p.spr = s.supp >> 9 & 1;
        p.scc = s.supp >> 8 & 1;
        pal = s.colors == 0 ? (uint32_t)(w >> 12 & 0xF) | (s.supp >> 1 & 0x70) : (uint32_t)(w >> 8 & 0x70);
        if (!s.cnsm) {
            hf = w & 0x400; vf = w & 0x800;
            chr = s.cell2x2 ? ((w & 0x3FFu) << 2) | (s.supp & 3u) | ((s.supp & 0x1Cu) << 10)
                            : (w & 0x3FFu) | ((s.supp & 0x1Fu) << 10);
        } else {
            hf = vf = false;
            chr = s.cell2x2 ? ((w & 0xFFFu) << 2) | (s.supp & 3u) | ((s.supp & 0x10u) << 10)
                            : (w & 0xFFFu) | ((s.supp & 0x1Cu) << 10);
        }
    } else {
        uint16_t w0 = vw(pnd_addr), w1 = vw(pnd_addr + 2);
        hf = w0 & 0x4000; vf = w0 & 0x8000;
        p.spr = w0 >> 13 & 1;
        p.scc = w0 >> 12 & 1;
        pal = w0 & 0x7F;
        chr = w1 & 0x7FFF;
    }
    int px = (int)(ix % (uint32_t)cs), py = (int)(iy % (uint32_t)cs);
    if (hf) px = cs - 1 - px;
    if (vf) py = cs - 1 - py;
    static const uint32_t kCellBytes[] = {32, 64, 128, 128, 256};
    uint32_t base = chr * 0x20;
    if (s.cell2x2) base += (uint32_t)((py >> 3) * 2 + (px >> 3)) * kCellBytes[s.colors];
    return dot(s, base, px & 7, py & 7, 8, pal, p);
}

// ---- the sprite layer -----------------------------------------------------------------------
struct SpriteType { int pr_shift, pr_mask, cc_shift, cc_mask, dc_mask; };
static const SpriteType kTypes[16] = {
    {14, 3, 11, 7, 0x7FF}, {13, 7, 11, 3, 0x7FF}, {14, 1, 11, 7, 0x7FF}, {13, 3, 11, 3, 0x7FF},
    {13, 3, 10, 7, 0x3FF}, {12, 7, 11, 1, 0x7FF}, {12, 7, 10, 3, 0x3FF}, {12, 7, 9, 7, 0x1FF},
    {7, 1, 0, 0, 0x7F},    {7, 1, 6, 1, 0x3F},    {6, 3, 0, 0, 0x3F},    {0, 0, 6, 3, 0x3F},
    {7, 1, 0, 0, 0xFF},    {7, 1, 6, 1, 0xFF},    {6, 3, 0, 0, 0xFF},    {0, 0, 6, 3, 0xFF},
};

// S_SHADOW is a dot that shows nothing itself and halves the brightness of what is under it.
enum SpriteDot { S_NONE, S_DOT, S_SHADOW };

static SpriteDot sprite_pixel(uint16_t d, Pix& p) {
    uint16_t spctl = reg(0xE0);
    int type = spctl & 0xF;
    bool window = spctl >> 4 & 1;
    bool mixed = spctl >> 5 & 1;
    int ccmode = spctl >> 12 & 3, ccnum = spctl >> 8 & 7;
    if (d == 0) return S_NONE;
    auto prio_of = [](int i) { return (uint8_t)(reg(0xF0 + (i / 2) * 2) >> ((i & 1) * 8) & 7); };
    auto ratio_of = [](int i) { return (uint8_t)(reg(0x100 + (i / 2) * 2) >> ((i & 1) * 8) & 0x1F); };
    bool msb;
    SpriteDot kind = S_DOT;
    if (mixed && (d & 0x8000)) {
        p.rgb = rgb555(d);
        p.prio = prio_of(0);
        p.ratio = ratio_of(0);
        msb = true;
    } else {
        const SpriteType& t = kTypes[type];
        if (type >= 8) d &= 0xFF;
        uint16_t dc = d & t.dc_mask;
        // Types 2-7 leave the MSB free, for the sprite window when SPWINEN is set and for shadows when not.
        bool msb_shadow = type >= 2 && type <= 7 && !window && (d & 0x8000);
        if (dc == t.dc_mask - 1) kind = S_SHADOW;                       // a normal shadow
        else if (msb_shadow && !(d & 0x7FFF)) {
            if (!(reg(0xE2) >> 8 & 1)) return S_NONE;                    // transparent shadows off (TPSDSL)
            kind = S_SHADOW;
        } else if (dc == 0) return S_NONE;
        p.shade = msb_shadow;
        int pr = t.pr_mask ? d >> t.pr_shift & t.pr_mask : 0;
        int cc = t.cc_mask ? d >> t.cc_shift & t.cc_mask : 0;
        p.prio = prio_of(pr);
        p.ratio = ratio_of(cc);
        uint32_t caos = (uint32_t)(reg(0xE6) >> 4 & 7) << 8;
        p.rgb = cram(dc + caos, &msb);
    }
    switch (ccmode) {
    case 0: p.cc = p.prio <= ccnum; break;
    case 1: p.cc = p.prio == ccnum; break;
    case 2: p.cc = p.prio >= ccnum; break;
    default: p.cc = msb; break;
    }
    p.cc = p.cc && (reg(0xEC) >> 6 & 1);
    p.offset = reg(0x110) >> 6 & 1;
    p.offset_b = reg(0x112) >> 6 & 1;
    return p.prio ? kind : S_NONE;
}

// ---- windows ----------------------------------------------------------------------------------
// Window 0 or 1 on one line: the line in its Y range, and its X span there.
struct Win { bool on_line; int xs, xe; };

static Win window_line(int w, int y, bool dd) {
    uint32_t b = 0xC0 + w * 8;
    int xs = reg(b) & 0x3FF, xe = reg(b + 4) & 0x3FF;
    uint16_t lwta = reg(0xD8 + w * 4);
    if (lwta & 0x8000) {                         // a line window: a span a line from the table
        uint32_t a = (((uint32_t)(lwta & 7) << 16 | reg(0xDA + w * 4)) & 0x7FFFE) * 2 + (uint32_t)y * 4;
        xs = vw(a) & 0x3FF;
        xe = vw(a + 2) & 0x3FF;
    }
    if (!(reg(0x00) & 2)) { xs >>= 1; xe >>= 1; } // X counts half dots below the high resolutions
    int ys = reg(b + 2) & 0x3FF, ye = reg(b + 6) & 0x3FF;
    if (!dd) { ys &= 0x1FF; ye &= 0x1FF; }
    return {y >= ys && y <= ye, xs, xe};
}

// Whether a layer's window control byte (WCTLA-D) makes dot x transparent: each enabled window's
// area (bit 0 or 2: inside when clear, outside when set), joined by OR or by AND (bit 7).
static bool windowed(uint8_t ctl, const Win w[2], int x) {
    bool any = false, all = true, used = false;
    for (int i = 0; i < 2; ++i) {
        if (!(ctl >> (i * 2 + 1) & 1)) continue;
        bool in = w[i].on_line && x >= w[i].xs && x <= w[i].xe;
        bool area = (ctl >> (i * 2) & 1) ? !in : in;
        any |= area;
        all &= area;
        used = true;
    }
    return used && (ctl & 0x80 ? all : any);
}

// ---- composing ------------------------------------------------------------------------------
static void note_unsupported() {
    static bool noted[8];
    bool fresh = false;
    auto once = [&](int i, bool cond, const char* what, uint16_t v) {
        if (cond && !noted[i]) { noted[i] = fresh = true; sat_note("VDP2: %s (%04X) is not done", what, v); }
    };
    once(0, reg(0x20) & 0x30, "RBG0/RBG1", reg(0x20));
    once(1, reg(0x9A) & 0x3F3F, "line or vertical cell scroll (SCRCTL)", reg(0x9A));
    once(2, reg(0x22) & 0xF, "mosaic (MZCTL)", reg(0x22));
    once(3, (reg(0xD0) | reg(0xD2) | reg(0xD4) | reg(0xD6)) & 0x2020, "the sprite window (WCTL)", reg(0xD0));
    once(4, reg(0xEC) & 0x8600 && reg(0xEC) & 0x5F, "extended colour calculation or gradation (CCCTL)", reg(0xEC));
    once(5, (reg(0x00) & 7) >= 4, "an exclusive resolution (TVMD)", reg(0x00));
    auto mode = [](uint16_t r, int m) {
        for (int n = 0; n < 4; ++n) if ((r >> (n * 2) & 3) == m) return true;
        return false;
    };
    once(6, reg(0xE8) & 0x3F, "the line colour screen (LNCLEN)", reg(0xE8));
    once(7, mode(reg(0xEA), 2) || mode(reg(0xEE), 2), "special priority or colour calculation by dot (SFPRMD, SFCCMD)", reg(0xEA));
}

static uint32_t apply_offset(uint32_t rgb, bool b) {
    uint32_t base = b ? 0x11A : 0x114;
    int out = 0;
    for (int i = 0; i < 3; ++i) {
        int o = (int)((uint32_t)(reg(base + i * 2) & 0x1FF) << 23) >> 23;
        int c = (int)(rgb >> (16 - i * 8) & 0xFF) + o;
        out = out << 8 | std::clamp(c, 0, 255);
    }
    return (uint32_t)out;
}

void vdp2_compose(Frame& f) {
    uint16_t tvmd = reg(0x00);
    static const int kWidths[4] = {320, 352, 640, 704};
    int hreso = tvmd & 7;
    f.w = kWidths[hreso < 4 ? hreso : hreso & 1];
    bool dd = (tvmd >> 6 & 3) == 3;              // double-density interlace
    f.h = vdp2_lines() * (dd ? 2 : 1);
    f.px.assign((size_t)f.w * f.h, 0);
    if (!(tvmd & 0x8000)) return;                // display off
    note_unsupported();
    Nbg nbg[4];
    for (int n = 0; n < 4; ++n) setup_nbg(n, nbg[n]);
    uint32_t bk = ((uint32_t)(reg(0xAC) & 7) << 16 | reg(0xAE)) * 2;
    bool bk_lines = reg(0xAC) & 0x8000;
    bool add = reg(0xEC) >> 8 & 1;
    Pix back{};
    back.offset = reg(0x110) >> 5 & 1;
    back.offset_b = reg(0x112) >> 5 & 1;
    uint16_t sfprmd = reg(0xEA), sfccmd = reg(0xEE);
    uint8_t wctl[L_COUNT] = {(uint8_t)(reg(0xD4) >> 8), (uint8_t)reg(0xD0), (uint8_t)(reg(0xD0) >> 8),
                             (uint8_t)reg(0xD2), (uint8_t)(reg(0xD2) >> 8)};
    uint8_t cc_wctl = (uint8_t)(reg(0xD6) >> 8);
    uint16_t sdctl = reg(0xE2);
    for (int y = 0; y < f.h; ++y) {
        int line = dd ? y >> 1 : y;
        Win win[2] = {window_line(0, y, dd), window_line(1, y, dd)};
        back.rgb = rgb555(vw(bk + (bk_lines ? (uint32_t)line * 2 : 0)));
        for (int x = 0; x < f.w; ++x) {
            Pix l[L_COUNT];
            int top = -1, second = -1;
            auto consider = [&](int i) {
                if (!l[i].prio) return;
                if (top < 0 || l[i].prio > l[top].prio) { second = top; top = i; }
                else if (second < 0 || l[i].prio > l[second].prio) second = i;
            };
            // in the order that wins ties: sprite, then NBG0..NBG3
            l[L_SPRITE] = {};
            uint8_t shadow = 0;                  // the priority of a shadow dot here
            if (!windowed(wctl[L_SPRITE], win, x)) {
                switch (sprite_pixel(vdp1_dot(x, y, dd), l[L_SPRITE])) {
                case S_DOT: consider(L_SPRITE); break;
                case S_SHADOW: shadow = l[L_SPRITE].prio; l[L_SPRITE] = {}; break;
                case S_NONE: break;
                }
            }
            for (int n = 0; n < 4; ++n) {
                Pix& p = l[L_NBG0 + n];
                p = {};
                const Nbg& s = nbg[n];
                if (!s.on || !s.prio || windowed(wctl[L_NBG0 + n], win, x)) continue;
                if (!nbg_pixel(s, x, y, p)) continue;
                p.prio = s.prio;
                if ((sfprmd >> (n * 2) & 3) == 1) p.prio = (uint8_t)((s.prio & 6) | p.spr);
                if (!p.prio) continue;
                switch (sfccmd >> (n * 2) & 3) {
                case 1: p.cc = s.cc && p.scc; break;
                case 3: p.cc = s.cc && p.msb; break;
                default: p.cc = s.cc; break;
                }
                p.ratio = s.ratio; p.offset = s.offset; p.offset_b = s.offset_b;
                consider(L_NBG0 + n);
            }
            const Pix& t = top >= 0 ? l[top] : back;
            const Pix& u = second >= 0 ? l[second] : back;
            uint32_t rgb = t.rgb;
            if (top >= 0 && t.cc && !windowed(cc_wctl, win, x)) {
                uint32_t out = 0;
                for (int sh = 16; sh >= 0; sh -= 8) {
                    int a = (int)(t.rgb >> sh & 0xFF), b = (int)(u.rgb >> sh & 0xFF);
                    int c = add ? std::min(a + b, 255) : (a * (32 - t.ratio) + b * t.ratio) >> 5;
                    out |= (uint32_t)c << sh;
                }
                rgb = out;
            }
            if (t.offset) rgb = apply_offset(rgb, t.offset_b);
            // A shadow halves the top screen under it when SDCTL enables that screen (bit 5: the back screen).
            bool shaded = top == L_SPRITE ? t.shade
                        : shadow && shadow >= t.prio && (sdctl >> (top >= 0 ? top - L_NBG0 : 5) & 1);
            if (shaded) rgb = rgb >> 1 & 0x7F7F7F;
            f.px[(size_t)y * f.w + x] = rgb;
        }
    }
}

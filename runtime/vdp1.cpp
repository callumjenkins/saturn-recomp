// saturnkit runtime — VDP1: the command table drawn in software into its two
// framebuffers, the frame change and the erase.
//
// A draw starts when PTMR is written with 1, or at a frame change with
// PTMR 2. It is done at once, when it starts: the command table as it is at
// that moment, from address 0, through the jumps (next, assign, call,
// return, and their skips) to the END bit. The game waits for the
// sprite-draw-end interrupt, 1 ms later (video.cpp's time), before it
// touches the table again. System clipping, user clipping and the local
// coordinates stay from one draw to the next, as on the chip.
//
// Every shape is a quadrilateral drawn the chip's way, as lines: its left
// edge A->D and its right edge B->C are walked in the same number of steps
// (the longer edge's length), and at each step a line joins them. Lines are
// Bresenham's with an extra pixel wherever both coordinates step (VDP1's
// anti-aliasing, so no holes between lines). A texture's row is the step's
// (v = i * h / steps), its column the position along the line
// (u = k * w / length). Normal and scaled sprites are quadrilaterals too.
// Polylines and lines are the lines alone.
//
// Pixels: the colour modes (4 bpp bank and lookup table, 8 bpp banks, RGB),
// transparent pixels (SPD), end codes (ECD), system and user clipping
// (inside or outside), mesh, MSB on, and the colour calculations: replace,
// shadow, half-luminance, half-transparency, each with Gouraud shading. The
// framebuffer is 16 bits a pixel, 512x256 (TVMR 0); 8 bits a pixel and
// rotation are not done.
//
// Frames (FBCR): the frame changes as the VBlank ends (VBlank-OUT), as in
// Mednafen: a game that asks for it in its VBlank-IN handler (Virtual
// Hydlide's movie player does, once a draw has ended) gets it in the same
// blanking. One-cycle mode (FCM 0) changes frame every field and erases the
// framebuffer it shows as it shows it; manual mode changes frame when FCT is
// 1 and erases in the next field when FCT is 0. The erase writes EWDR over EWLR-EWRR. The CPU sees
// the framebuffer being drawn.
#include "saturn.h"
#include "video.h"
#include <algorithm>
#include <cstdlib>

uint8_t g_vdp1_vram[0x80000];
static uint16_t g_fb[2][512 * 256];             // host order
static int g_draw_fb;                           // the other one is shown
static uint16_t g_reg[0x10];                    // TVMR FBCR PTMR EWDR EWLR EWRR ENDR - EDSR LOPR COPR MODR
enum { TVMR, FBCR, PTMR, EWDR, EWLR, EWRR, ENDR, EDSR = 8, LOPR, COPR, MODR };
static bool g_change_pending, g_erase_pending, g_erase_field;
static bool g_drawing;
static uint64_t g_draw_end, g_frame_changes, g_draws;

// the drawing state that outlives a draw
static int g_sys_x = 319, g_sys_y = 223;        // system clip: 0..x, 0..y
static int g_ux0, g_uy0, g_ux1 = 319, g_uy1 = 223;
static int g_local_x, g_local_y;

uint64_t vdp1_frame_changes() { return g_frame_changes; }
uint64_t vdp1_draws() { return g_draws; }
const uint16_t* vdp1_display() { return g_fb[g_draw_fb ^ 1]; }

void vdp1_init() {
    g_reg[MODR] = 0x1000;                       // version 1
    g_reg[EDSR] = 0x0002;                       // the last draw has ended
}

static uint16_t vw(uint32_t a) {
    a &= 0x7FFFE;
    return (uint16_t)(g_vdp1_vram[a] << 8 | g_vdp1_vram[a + 1]);
}
static int sx13(uint16_t v) { return (int32_t)((uint32_t)v << 19) >> 19; }
static int sx11(uint16_t v) { return (int32_t)((uint32_t)v << 21) >> 21; }

// ---- one command -------------------------------------------------------------------------
struct Pt { int x, y; };
struct Rgb { int r, g, b; };                     // Gouraud values, 5 bits, 16 is neutral

struct Cmd {
    uint16_t ctrl, pmod, colr;
    uint32_t srca;
    int w, h;                                   // texture
    int cmode, ccb;
    bool textured, spd, ecd, mesh, clip, outside, mon, gouraud, hflip, vflip;
    uint16_t lut[16];
    Rgb g[4];                                   // A B C D
};

static Rgb rgb_of(uint16_t v) { return {v & 0x1F, v >> 5 & 0x1F, v >> 10 & 0x1F}; }

// The texel at (u, v): false if it is not drawn (transparent, an end code).
static bool texel(const Cmd& c, int u, int v, uint16_t& out, bool& end) {
    if (c.hflip) u = c.w - 1 - u;
    if (c.vflip) v = c.h - 1 - v;
    uint32_t i = (uint32_t)(v * c.w + u);
    uint32_t raw;
    bool is_end, is_zero;
    switch (c.cmode) {
    case 0: case 1: {
        uint8_t b = g_vdp1_vram[(c.srca + i / 2) & 0x7FFFF];
        raw = (i & 1) ? (b & 0xF) : (b >> 4);
        is_end = raw == 0xF;
        is_zero = raw == 0;
        out = c.cmode == 0 ? (uint16_t)((c.colr & 0xFFF0) | raw) : c.lut[raw];
        break;
    }
    case 2: case 3: case 4: {
        raw = g_vdp1_vram[(c.srca + i) & 0x7FFFF];
        is_end = raw == 0xFF;
        is_zero = raw == 0;
        static const uint16_t kMask[] = {0x3F, 0x7F, 0xFF};
        uint16_t m = kMask[c.cmode - 2];
        out = (uint16_t)((c.colr & ~m) | (raw & m));
        break;
    }
    default:
        raw = vw(c.srca + i * 2);
        is_end = raw == 0x7FFF;
        is_zero = raw == 0;
        out = (uint16_t)raw;
        break;
    }
    end = false;
    if (is_end && !c.ecd) { end = true; return false; }
    return !(is_zero && !c.spd);
}

static void plot(const Cmd& c, int x, int y, uint16_t pix, const Rgb* g) {
    if (x < 0 || y < 0 || x > g_sys_x || y > g_sys_y || x >= 512 || y >= 256) return;
    if (c.clip) {
        bool in = x >= g_ux0 && x <= g_ux1 && y >= g_uy0 && y <= g_uy1;
        if (in == c.outside) return;
    }
    if (c.mesh && ((x ^ y) & 1)) return;
    uint16_t& d = g_fb[g_draw_fb][y * 512 + x];
    if (c.mon) { d |= 0x8000; return; }
    if (g && (pix & 0x8000)) {
        int r = std::clamp((pix & 0x1F) + g->r - 16, 0, 31);
        int gg = std::clamp((pix >> 5 & 0x1F) + g->g - 16, 0, 31);
        int b = std::clamp((pix >> 10 & 0x1F) + g->b - 16, 0, 31);
        pix = (uint16_t)(0x8000 | b << 10 | gg << 5 | r);
    }
    switch (c.ccb & 3) {
    case 0: d = pix; break;
    case 1:                                     // shadow: darken what is there, if it is RGB
        if (d & 0x8000) d = (uint16_t)(((d >> 1) & 0x3DEF) | 0x8000);
        break;
    case 2:                                     // half-luminance
        d = (pix & 0x8000) ? (uint16_t)(((pix >> 1) & 0x3DEF) | 0x8000) : pix;
        break;
    case 3:                                     // half-transparency over an RGB pixel
        if ((d & 0x8000) && (pix & 0x8000)) d = (uint16_t)(((uint32_t)pix + d - ((pix ^ d) & 0x8421)) >> 1);
        else d = pix;
        break;
    }
}

static Rgb mix(const Rgb& a, const Rgb& b, int k, int n) {
    if (n <= 0) return a;
    return {a.r + (b.r - a.r) * k / n, a.g + (b.g - a.g) * k / n, a.b + (b.b - a.b) * k / n};
}

// A line from p0 to p1; for a texture, row v, its columns spread along the line.
static bool outside_clip(const Cmd& c, int x0, int y0, int x1, int y1);

static void line(const Cmd& c, Pt p0, Pt p1, int v, Rgb g0, Rgb g1) {
    if (outside_clip(c, std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x), std::max(p0.y, p1.y))) return;
    int dx = p1.x - p0.x, dy = p1.y - p0.y;
    int adx = std::abs(dx), ady = std::abs(dy);
    int len = std::max(adx, ady);
    int xi = dx < 0 ? -1 : 1, yi = dy < 0 ? -1 : 1;
    int ends = 0;
    bool stop = false;
    auto pixel = [&](int x, int y, int k) {
        if (stop) return;
        uint16_t pix = c.colr;
        if (c.textured) {
            bool end;
            int u = (int)((int64_t)k * c.w / (len + 1));
            if (!texel(c, u, v, pix, end)) {
                if (end && ++ends == 2) stop = true;
                return;
            }
        }
        Rgb g;
        if (c.gouraud) g = mix(g0, g1, k, len);
        plot(c, x, y, pix, c.gouraud ? &g : nullptr);
    };
    int x = p0.x, y = p0.y;
    if (adx >= ady) {
        int err = 2 * ady - adx;
        for (int k = 0; k <= len; ++k) {
            pixel(x, y, k);
            if (k == len) break;
            if (err > 0) {
                y += yi;
                err -= 2 * adx;
                pixel(x, y, k);                 // anti-aliasing: the corner between the two steps
            }
            err += 2 * ady;
            x += xi;
        }
    } else {
        int err = 2 * adx - ady;
        for (int k = 0; k <= len; ++k) {
            pixel(x, y, k);
            if (k == len) break;
            if (err > 0) {
                x += xi;
                err -= 2 * ady;
                pixel(x, y, k);
            }
            err += 2 * adx;
            y += yi;
        }
    }
}

static int step_to(int a, int b, int i, int n) {   // a + (b - a) * i / n, rounded
    if (n <= 0) return a;
    int d = (b - a) * i * 2;
    return a + (d >= 0 ? (d + n) / (2 * n) : -((-d + n) / (2 * n)));
}

// the area a pixel can land in: system clipping, and the user clipping inside it
static bool outside_clip(const Cmd& c, int x0, int y0, int x1, int y1) {
    if (x1 < 0 || y1 < 0 || x0 > std::min(g_sys_x, 511) || y0 > std::min(g_sys_y, 255)) return true;
    return c.clip && !c.outside && (x1 < g_ux0 || y1 < g_uy0 || x0 > g_ux1 || y0 > g_uy1);
}

static void quad(const Cmd& c, Pt a, Pt b, Pt cc, Pt d) {
    if (outside_clip(c, std::min({a.x, b.x, cc.x, d.x}), std::min({a.y, b.y, cc.y, d.y}),
                     std::max({a.x, b.x, cc.x, d.x}), std::max({a.y, b.y, cc.y, d.y})))
        return;
    int left = std::max(std::abs(d.x - a.x), std::abs(d.y - a.y));
    int right = std::max(std::abs(cc.x - b.x), std::abs(cc.y - b.y));
    int n = std::max(left, right);
    if (n > 2048) return;                       // a shape far off the framebuffer: the chip would spend ages on it
    for (int i = 0; i <= n; ++i) {
        Pt l{step_to(a.x, d.x, i, n), step_to(a.y, d.y, i, n)};
        Pt r{step_to(b.x, cc.x, i, n), step_to(b.y, cc.y, i, n)};
        int v = c.textured ? (int)((int64_t)i * c.h / (n + 1)) : 0;
        Rgb gl{}, gr{};
        if (c.gouraud) { gl = mix(c.g[0], c.g[3], i, n); gr = mix(c.g[1], c.g[2], i, n); }
        line(c, l, r, v, gl, gr);
    }
}

static void command(uint32_t a) {
    Cmd c{};
    c.ctrl = vw(a);
    c.pmod = vw(a + 4);
    c.colr = vw(a + 6);
    c.srca = (uint32_t)vw(a + 8) * 8;
    uint16_t size = vw(a + 10);
    c.w = (size >> 8 & 0x3F) * 8;
    c.h = size & 0xFF;
    int comm = c.ctrl & 0xF;
    Pt p[4];
    for (int i = 0; i < 4; ++i) p[i] = {sx13(vw(a + 12 + i * 4)), sx13(vw(a + 14 + i * 4))};
    switch (comm) {
    case 0x8: case 0xB:                         // user clipping
        g_ux0 = vw(a + 12) & 0x3FF; g_uy0 = vw(a + 14) & 0x1FF;
        g_ux1 = vw(a + 20) & 0x3FF; g_uy1 = vw(a + 22) & 0x1FF;
        return;
    case 0x9:                                   // system clipping
        g_sys_x = vw(a + 20) & 0x3FF; g_sys_y = vw(a + 22) & 0x1FF;
        return;
    case 0xA:                                   // local coordinates
        g_local_x = sx11(vw(a + 12)); g_local_y = sx11(vw(a + 14));
        return;
    }
    if (comm > 7) { sat_trace("VDP1: command %X at %05X", comm, a); return; }
    c.cmode = c.pmod >> 3 & 7;
    c.ccb = c.pmod & 7;
    c.spd = c.pmod & 0x40;
    c.ecd = c.pmod & 0x80;
    c.mesh = c.pmod & 0x100;
    c.clip = c.pmod & 0x400;
    c.outside = c.pmod & 0x200;
    c.mon = c.pmod & 0x8000;
    c.gouraud = c.ccb & 4;
    c.hflip = c.ctrl & 0x10;
    c.vflip = c.ctrl & 0x20;
    c.textured = comm < 4;
    if (c.textured && (c.w == 0 || c.h == 0)) return;
    if (c.textured && c.cmode == 1)
        for (int i = 0; i < 16; ++i) c.lut[i] = vw((uint32_t)c.colr * 8 + i * 2);
    if (c.gouraud) {
        uint32_t ga = (uint32_t)vw(a + 28) * 8;
        for (int i = 0; i < 4; ++i) c.g[i] = rgb_of(vw(ga + i * 2));
    }
    for (Pt& q : p) { q.x += g_local_x; q.y += g_local_y; }
    switch (comm) {
    case 0x0: {                                 // normal sprite
        int x0 = p[0].x, y0 = p[0].y, x1 = x0 + c.w - 1, y1 = y0 + c.h - 1;
        quad(c, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1});
        break;
    }
    case 0x1: {                                 // scaled sprite
        int zp = c.ctrl >> 8 & 0xF;
        int x0, y0, x1, y1;
        if (!zp) {
            x0 = p[0].x; y0 = p[0].y; x1 = p[2].x; y1 = p[2].y;
        } else {
            int w = sx13(vw(a + 16)), h = sx13(vw(a + 18));   // B: the width and height on screen
            switch (zp & 3) {
            case 2: x0 = p[0].x - w / 2; x1 = x0 + w; break;
            case 3: x0 = p[0].x - w; x1 = p[0].x; break;
            default: x0 = p[0].x; x1 = p[0].x + w; break;
            }
            switch (zp >> 2 & 3) {
            case 2: y0 = p[0].y - h / 2; y1 = y0 + h; break;
            case 3: y0 = p[0].y - h; y1 = p[0].y; break;
            default: y0 = p[0].y; y1 = p[0].y + h; break;
            }
        }
        quad(c, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1});
        break;
    }
    case 0x2: case 0x3: case 0x4:              // distorted sprite, polygon
        quad(c, p[0], p[1], p[2], p[3]);
        break;
    case 0x5: case 0x7:                         // polyline
        for (int i = 0; i < 4; ++i) line(c, p[i], p[(i + 1) & 3], 0, c.g[i], c.g[(i + 1) & 3]);
        break;
    case 0x6:                                   // line
        line(c, p[0], p[1], 0, c.g[0], c.g[1]);
        break;
    }
}

static void draw() {
    g_reg[EDSR] = (uint16_t)(g_reg[EDSR] >> 1 & 1);   // BEF <- CEF, CEF <- 0
    if (g_reg[TVMR] & 3) sat_fatal("VDP1: TVMR %04X (8 bpp or rotation) is not done", g_reg[TVMR]);
    uint32_t a = 0, ret = 0, last = 0;
    int n = 0;
    for (int guard = 0; guard < 20000; ++guard) {
        uint16_t ctrl = vw(a);
        last = a;
        if (ctrl & 0x8000) break;
        int jp = ctrl >> 12 & 7;
        if (!(jp & 4)) { command(a); ++n; }
        uint32_t link = (uint32_t)vw(a + 2) * 8 & 0x7FFFF;
        switch (jp & 3) {
        case 0: a += 0x20; break;
        case 1: a = link; break;
        case 2: ret = a + 0x20; a = link; break;
        case 3: a = ret; break;
        }
        a &= 0x7FFFF;
    }
    g_reg[LOPR] = (uint16_t)(last >> 3);
    g_reg[COPR] = (uint16_t)(last >> 3);
    ++g_draws;
    g_drawing = true;
    g_draw_end = sat_now() + 1000000;           // 1 ms
    sat_trace("VDP1 draw %llu: %d commands", (unsigned long long)g_draws, n);
}

void vdp1_tick(uint64_t now) {
    if (g_drawing && now >= g_draw_end) {
        g_drawing = false;
        g_reg[EDSR] |= 2;                        // CEF
        scu_raise(IRQ_SPRITE_END);
        scu_frame_event(6);
    }
}

// ---- frames -------------------------------------------------------------------------------
static void erase(int which) {
    int x0 = (g_reg[EWLR] >> 9 & 0x7F) * 8, y0 = g_reg[EWLR] & 0x1FF;
    int x1 = (g_reg[EWRR] >> 9 & 0x7F) * 8, y1 = g_reg[EWRR] & 0x1FF;
    for (int y = y0; y <= y1 && y < 256; ++y)
        for (int x = x0; x < x1 && x < 512; ++x) g_fb[which][y * 512 + x] = g_reg[EWDR];
}

void vdp1_vblank_out() {
    // the field that has just been shown erased the framebuffer shown, if it had to
    if (g_erase_field) { erase(g_draw_fb ^ 1); g_erase_field = false; }
    bool one_cycle = !(g_reg[FBCR] & 2);
    if (g_erase_pending) { g_erase_field = true; g_erase_pending = false; }
    if (one_cycle || g_change_pending) {
        g_change_pending = false;
        g_draw_fb ^= 1;
        ++g_frame_changes;
        if (one_cycle) g_erase_field = true;
        sat_trace("VDP1 frame change %llu", (unsigned long long)g_frame_changes);
        if ((g_reg[PTMR] & 3) == 2) draw();
    }
}

// ---- the bus ------------------------------------------------------------------------------
uint32_t vdp1_reg_read(uint32_t off, int size) {
    if (size == 4) return vdp1_reg_read(off, 2) << 16 | vdp1_reg_read(off + 2, 2);
    uint16_t v = g_reg[(off >> 1) & 0xF];
    return size == 1 ? (off & 1 ? v & 0xFF : v >> 8) : v;
}

static void reg_write(uint32_t off, uint16_t v) {
    int r = (off >> 1) & 0xF;
    sat_trace("VDP1 reg %02X <- %04X", off, v);
    if (r >= EDSR) return;                       // read-only
    g_reg[r] = v;
    switch (r) {
    case FBCR:
        if ((v & 3) == 3) g_change_pending = true;
        if ((v & 3) == 2) g_erase_pending = true;
        break;
    case PTMR:
        if ((v & 3) == 1) draw();
        break;
    }
}

void vdp1_reg_write(uint32_t off, uint32_t v, int size) {
    if (size == 4) { reg_write(off, (uint16_t)(v >> 16)); reg_write(off + 2, (uint16_t)v); }
    else if (size == 2) reg_write(off, (uint16_t)v);
    else sat_fatal("byte write to VDP1 register %02X", off);
}

uint32_t vdp1_fb_read(uint32_t off, int size) {
    const uint16_t* fb = g_fb[g_draw_fb];
    if (size == 4) return (uint32_t)fb[(off >> 1) & 0x1FFFF] << 16 | fb[((off >> 1) + 1) & 0x1FFFF];
    uint16_t v = fb[(off >> 1) & 0x1FFFF];
    return size == 1 ? (off & 1 ? v & 0xFF : v >> 8) : v;
}

void vdp1_fb_write(uint32_t off, uint32_t v, int size) {
    uint16_t* fb = g_fb[g_draw_fb];
    if (size == 4) { fb[(off >> 1) & 0x1FFFF] = (uint16_t)(v >> 16); fb[((off >> 1) + 1) & 0x1FFFF] = (uint16_t)v; return; }
    uint16_t& w = fb[(off >> 1) & 0x1FFFF];
    if (size == 2) w = (uint16_t)v;
    else w = off & 1 ? (uint16_t)((w & 0xFF00) | (v & 0xFF)) : (uint16_t)((w & 0x00FF) | (v & 0xFF) << 8);
}

void vdp1_dump(FILE* f) {
    std::fwrite(g_vdp1_vram, 1, sizeof g_vdp1_vram, f);
    for (uint16_t p : g_fb[g_draw_fb]) { uint8_t b[2] = {(uint8_t)(p >> 8), (uint8_t)p}; std::fwrite(b, 1, 2, f); }
    for (uint16_t r : g_reg) { uint8_t b[2] = {(uint8_t)(r >> 8), (uint8_t)r}; std::fwrite(b, 1, 2, f); }
}

// saturn-recomp runtime — VDP1's shapes drawn to pixels by Mednafen's rules, included by
// vdp1.cpp when the runtime is built with SATURN_VDP1_GPL.
//
// This file is not under saturn-recomp's MIT licence. It follows the VDP1 of Mednafen 1.32.1
// (src/ss/vdp1.cpp, vdp1_common.h, vdp1_sprite.cpp, vdp1_poly.cpp, vdp1_line.cpp;
// Copyright (C) 2015-2020 Mednafen Team), and like Mednafen it is free software under the GNU
// General Public License, version 2 or (at your option) any later version. A build that includes
// it is covered by the GPL as a whole. See THIRD_PARTY.md.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// The rules: a sprite or polygon is drawn as lines between its left edge A->D and its right edge
// B->C, one line for each step of the longer edge. Each edge, each line and each texture
// coordinate steps on an error term of its own, whose rounding depends on the direction of
// travel, kept in 13 bits as the chip keeps it. A line's anti-aliasing pixel goes on the side its
// direction decides. A line wholly outside the clipping is drawn as its first point only, and
// only for a shape's last line; one that has been inside the clipping stops when it leaves.
// Lines and polylines are lines alone, without anti-aliasing. Coordinates wrap at 11 bits.

namespace {

int32_t s13(int32_t v) { return (int32_t)((uint32_t)v << 19) >> 19; }

struct Vtx { int32_t x, y; uint16_t g; int32_t t; };

// A Gouraud colour, 5 bits a channel, stepped from one end's to the other's over `length` pixels.
struct Shade {
    uint32_t g = 0, intinc = 0;
    int32_t ginc[3]{}, error[3]{}, error_inc[3]{}, error_adj[3]{};

    void setup(unsigned length, uint16_t from, uint16_t to) {
        g = from & 0x7FFF;
        intinc = 0;
        for (int cc = 0; cc < 3; ++cc) {
            int dg = ((to >> (cc * 5)) & 0x1F) - ((from >> (cc * 5)) & 0x1F);
            unsigned adg = (unsigned)std::abs(dg);
            ginc[cc] = (int32_t)((uint32_t)(dg >= 0 ? 1 : -1) << (cc * 5));
            if (length <= adg) {
                error_inc[cc] = (int32_t)(adg + 1) * 2;
                error_adj[cc] = (int32_t)length * 2;
                error[cc] = (int32_t)(adg + 1) - ((int32_t)length * 2 + (dg < 0 ? 1 : 0));
                while (error[cc] >= 0) { g += ginc[cc]; error[cc] -= error_adj[cc]; }
                while (error_inc[cc] >= error_adj[cc]) { intinc += ginc[cc]; error_inc[cc] -= error_adj[cc]; }
            } else {
                error_inc[cc] = (int32_t)adg * 2;
                error_adj[cc] = ((int32_t)length - 1) * 2;
                error[cc] = (int32_t)length - ((int32_t)length * 2 - (dg < 0 ? 1 : 0));
                if (error[cc] >= 0) { g += ginc[cc]; error[cc] -= error_adj[cc]; }
                if (error_inc[cc] >= error_adj[cc]) { intinc += ginc[cc]; error_inc[cc] -= error_adj[cc]; }
            }
            error[cc] = ~error[cc];
        }
    }

    void step() {
        g += intinc;
        for (int cc = 0; cc < 3; ++cc) {
            error[cc] -= error_inc[cc];
            uint32_t mask = (uint32_t)(error[cc] >> 31);
            g += (uint32_t)ginc[cc] & mask;
            error[cc] += error_adj[cc] & (int32_t)mask;
        }
    }

    Rgb rgb() const { return {(int)(g & 0x1F), (int)(g >> 5 & 0x1F), (int)(g >> 10 & 0x1F)}; }
};

// A texture coordinate (or row) stepped from `from` to `to` over `length` pixels, `sf` a step.
struct TexStep {
    int32_t t = 0, tinc = 0, error = 0, error_inc = 0, error_adj = 0;

    void setup(unsigned length, int32_t from, int32_t to, int32_t sf = 1, int32_t fudge = 0) {
        int32_t dt = to - from;
        unsigned adt = (unsigned)std::abs(dt);
        t = (from * sf) | fudge;
        tinc = dt >= 0 ? sf : -sf;
        if (length <= adt) {
            error_inc = (int32_t)(adt + 1) * 2;
            error_adj = (int32_t)length * 2;
            error = (int32_t)(adt + 1) - ((int32_t)length * 2 + (dt < 0 ? 1 : 0));
        } else {
            error_inc = (int32_t)adt * 2;
            error_adj = ((int32_t)length - 1) * 2;
            error = (int32_t)length - ((int32_t)length * 2 - (dt < 0 ? 1 : 0));
        }
    }
    bool pending() const { return error >= 0; }
    int32_t inc() { t += tinc; error -= error_adj; return t; }
    void add() { error += error_inc; }
    int32_t prestep() {
        while (error >= 0) { t += tinc; error -= error_adj; }
        error += error_inc;
        return t;
    }
};

// An edge from p0 to p1, stepped once for each of the shape's `dmax` + 1 lines.
struct Edge {
    uint32_t d_error, d_error_inc, d_error_adj; int32_t d_error_cmp;
    uint32_t x, x_inc, x_error, x_error_inc, x_error_adj; int32_t x_error_cmp;
    uint32_t y, y_inc, y_error, y_error_inc, y_error_adj; int32_t y_error_cmp;
    Shade g;
    bool gouraud;

    void setup(bool gour, const Vtx& p0, const Vtx& p1, int32_t dmax) {
        gouraud = gour;
        int32_t dx = s13(p1.x - p0.x), dy = s13(p1.y - p0.y);
        int32_t adx = std::abs(dx), ady = std::abs(dy), m = std::max(adx, ady);
        x = (uint32_t)p0.x;
        x_inc = dx >= 0 ? 1u : (uint32_t)-1;
        x_error_inc = (uint32_t)(2 * adx);
        x_error_adj = (uint32_t)-(2 * m);
        x_error = (uint32_t)((m - 2 * m) - 1);
        x_error_cmp = dy < 0 ? -1 : 0;
        y = (uint32_t)p0.y;
        y_inc = dy >= 0 ? 1u : (uint32_t)-1;
        y_error_inc = (uint32_t)(2 * ady);
        y_error_adj = (uint32_t)-(2 * m);
        y_error = (uint32_t)((m - 2 * m) - 1);
        y_error_cmp = dx < 0 ? -1 : 0;
        d_error = (uint32_t)(dmax - 2 * dmax - 1);
        d_error_inc = (uint32_t)(2 * m);
        d_error_adj = (uint32_t)-(2 * dmax);
        d_error_cmp = ((ady > adx ? dy : dx) < 0) ? -1 : 0;
        for (uint32_t* v : {&x_error, &x_error_inc, &x_error_adj, &y_error, &y_error_inc, &y_error_adj,
                            &d_error, &d_error_inc, &d_error_adj})
            *v <<= 19;
        x_error_cmp = (int32_t)((uint32_t)x_error_cmp << 19);
        y_error_cmp = (int32_t)((uint32_t)y_error_cmp << 19);
        d_error_cmp = (int32_t)((uint32_t)d_error_cmp << 19);
        if (gouraud) g.setup((unsigned)m + 1, p0.g, p1.g);
    }

    Vtx vertex() const { return {(int32_t)x, (int32_t)y, gouraud ? (uint16_t)g.g : (uint16_t)0, 0}; }

    void step() {
        d_error += d_error_inc;
        if ((int32_t)d_error >= d_error_cmp) {
            d_error += d_error_adj;
            x_error += x_error_inc;
            uint32_t xm = (uint32_t)-(int32_t)((int32_t)x_error >= x_error_cmp);
            x += x_inc & xm;
            x_error += x_error_adj & xm;
            y_error += y_error_inc;
            uint32_t ym = (uint32_t)-(int32_t)((int32_t)y_error >= y_error_cmp);
            y += y_inc & ym;
            y_error += y_error_adj & ym;
            if (gouraud) g.step();
        }
    }
};

// One line from p0 to p1; a texture's row `row`, its columns stepped from p0.t to p1.t. Returns
// whether pre-clipping found it wholly outside, when it is drawn only if `always` is set, as a
// point.
bool raster_line(const Cmd& c, Vtx p0, Vtx p1, bool aa, int row, bool always) {
    const bool pcd = c.pmod & 0x800, hss = c.pmod & 0x1000;
    const bool uclip_en = c.clip, uclip_out = c.outside;
    p0.x &= 0x1FFF; p0.y &= 0x1FFF; p1.x &= 0x1FFF; p1.y &= 0x1FFF;
    bool clipped = false;
    if (!pcd) {
        bool swapped;
        if (uclip_en && !uclip_out) {
            clipped |= ((((g_ux1 - p0.x) & (g_ux1 - p1.x)) | ((p0.x - g_ux0) & (p1.x - g_ux0))) & 0x1000) != 0;
            clipped |= ((((g_uy1 - p0.y) & (g_uy1 - p1.y)) | ((p0.y - g_uy0) & (p1.y - g_uy0))) & 0x1000) != 0;
            swapped = p0.y == p1.y && (p0.x < g_ux0 || p0.x > g_ux1);
        } else {
            clipped |= ((((g_sys_x - p0.x) & (g_sys_x - p1.x)) | (p0.x & p1.x)) & 0x1000) != 0;
            clipped |= ((((g_sys_y - p0.y) & (g_sys_y - p1.y)) | (p0.y & p1.y)) & 0x1000) != 0;
            swapped = p0.y == p1.y && p0.x > g_sys_x;
        }
        if (clipped) p1 = p0;
        else if (swapped) std::swap(p0, p1);
    }
    if (clipped && !always) return true;

    const int32_t dx = s13(p1.x - p0.x), dy = s13(p1.y - p0.y);
    const int32_t adx = std::abs(dx), ady = std::abs(dy), len = std::max(adx, ady);
    const int32_t xinc = dx >= 0 ? 1 : -1, yinc = dy >= 0 ? 1 : -1;
    const uint32_t step_x = (uint32_t)(xinc & 0x7FF), step_y = (uint32_t)(yinc & 0x7FF) << 16;
    uint32_t xy = (uint32_t)(p0.x & 0x7FF) | (uint32_t)(p0.y & 0x7FF) << 16;
    const uint32_t term = (uint32_t)(p1.x & 0x7FF) | (uint32_t)(p1.y & 0x7FF) << 16;
    bool drawn_ac = true;                       // every pixel so far was clipped

    Shade shade;
    if (c.gouraud) shade.setup((unsigned)len + 1, p0.g, p1.g);

    Cmd tc = c;                                 // the texture as stored: flips are in the coordinates
    tc.hflip = tc.vflip = false;
    TexStep t;
    int ec = 2;                                 // end codes left before the line stops
    uint16_t tex_pix = 0;
    bool tex_drawn = false;
    auto fetch = [&](int32_t tx) {
        bool end;
        tex_drawn = texel(tc, tx, row, tex_pix, end);
        if (end) --ec;
    };
    if (c.textured) {
        if (len < std::abs(p1.t - p0.t) && hss) {
            ec = 0x7FFFFFFF;
            t.setup((unsigned)len + 1, p0.t >> 1, p1.t >> 1, 2, (g_reg[FBCR] & 0x10) ? 1 : 0);
        } else {
            t.setup((unsigned)len + 1, p0.t, p1.t);
        }
        fetch(t.t);
    }

    int32_t ax, ay;                             // where the anti-aliasing pixel goes from the one before
    if (ady > adx) {
        if (yinc < 0) { ax = xinc >> 31; ay = -(xinc >> 31); }
        else { ax = -(~xinc >> 31); ay = ~xinc >> 31; }
    } else {
        if (xinc < 0) { ax = -(~yinc >> 31); ay = -(~yinc >> 31); }
        else { ax = yinc >> 31; ay = yinc >> 31; }
    }
    const uint32_t aa_step = (uint32_t)(ax & 0x7FF) | (uint32_t)(ay & 0x7FF) << 16;

    int32_t err, err_inc, err_adj, cmp = 0;
    uint32_t step_major, step_minor;
    if (ady > adx) {
        err_inc = 2 * adx; err_adj = -2 * ady; err = (ady - 2 * ady) - 1;
        if (dy < 0 && !aa) --cmp;
        err -= err_inc;
        xy = (xy + (0x8000000 - step_y)) & 0x07FF07FF;
        step_major = step_y; step_minor = step_x;
    } else {
        err_inc = 2 * ady; err_adj = -2 * adx; err = (adx - 2 * adx) - 1;
        if (dx < 0 && !aa) --cmp;
        err -= err_inc;
        xy = (xy + (0x800 - step_x)) & 0x07FF07FF;
        step_major = step_x; step_minor = step_y;
    }
    if (aa) { ++err; ++cmp; }
    uint32_t e = (uint32_t)err << 19;
    const uint32_t e_inc = (uint32_t)err_inc << 19, e_adj = (uint32_t)err_adj << 19;
    const int32_t e_cmp = (int32_t)((uint32_t)cmp << 19);

    const uint32_t sys = (uint32_t)(g_sys_y & 0x3FF) << 16 | (uint32_t)(g_sys_x & 0x3FF);
    const uint32_t u0 = (uint32_t)(g_uy0 & 0x3FF) << 16 | (uint32_t)(g_ux0 & 0x3FF);
    const uint32_t u1 = (uint32_t)(g_uy1 & 0x3FF) << 16 | (uint32_t)(g_ux1 & 0x3FF);
    // false: the line has left the clipping after being inside it, and ends
    auto pixel = [&](uint32_t pxy, uint16_t pix, bool transparent) {
        bool out = (uclip_en && !uclip_out) ? (((u1 - pxy) | (pxy - u0)) & 0x80008000) != 0
                                            : ((sys - pxy) & 0x80008000) != 0;
        if (out && !drawn_ac) return false;
        drawn_ac &= out;
        if (uclip_en) {
            if (!uclip_out) out |= ((sys - pxy) & 0x80008000) != 0;
            else out |= !(((u1 - pxy) | (pxy - u0)) & 0x80008000);
        }
        if (!transparent && !out) {
            Rgb g = shade.rgb();
            plot_clipped(c, (int)(pxy & 0x7FF), (int)(pxy >> 16 & 0x7FF), pix, c.gouraud ? &g : nullptr);
        }
        return true;
    };

    uint16_t pix = c.colr;
    bool transparent = false;                   // a polygon or line: drawn in its colour
    do {
        if (c.textured) {
            while (t.pending()) {
                fetch(t.inc());
                if (!c.ecd && ec <= 0) return clipped;
            }
            t.add();
            pix = tex_pix;
            transparent = !tex_drawn;
        }
        xy = (xy + step_major) & 0x07FF07FF;
        e += e_inc;
        if ((int32_t)e >= e_cmp) {
            e += e_adj;
            if (aa && !pixel((xy + aa_step) & 0x07FF07FF, pix, transparent)) return clipped;
            xy = (xy + step_minor) & 0x07FF07FF;
        }
        if (!pixel(xy, pix, transparent)) return clipped;
        if (c.gouraud) shade.step();
    } while (xy != term);
    return clipped;
}

uint16_t packed(const Rgb& g) { return (uint16_t)(g.r | g.g << 5 | g.b << 10); }

// A sprite or polygon with corners p[0] A, p[1] B, p[2] C, p[3] D.
void raster_quad(const Cmd& c, const Vtx* p, bool textured) {
    int32_t dmax = std::abs(s13(p[3].x - p[0].x));
    dmax = std::max(dmax, std::abs(s13(p[3].y - p[0].y)));
    dmax = std::max(dmax, std::abs(s13(p[2].x - p[1].x)));
    dmax = std::max(dmax, std::abs(s13(p[2].y - p[1].y)));
    dmax &= 0xFFF;
    Edge left, right;
    left.setup(c.gouraud, p[0], p[3], dmax);
    right.setup(c.gouraud, p[1], p[2], dmax);
    TexStep rows;
    if (textured) {
        int32_t top = c.vflip ? (c.h ? c.h - 1 : 0) : 0, bottom = c.vflip ? 0 : (c.h ? c.h - 1 : 0);
        rows.setup((unsigned)dmax + 1, top, bottom);
    }
    const int32_t t0 = c.hflip ? (c.w ? c.w - 1 : 0) : 0, t1 = c.hflip ? 0 : (c.w ? c.w - 1 : 0);
    for (int32_t iter = dmax; iter >= 0; --iter) {
        Vtx a = left.vertex(), b = right.vertex();
        a.t = t0;
        b.t = t1;
        int row = textured ? rows.prestep() : 0;
        raster_line(c, a, b, true, row, iter == 0);
        left.step();
        right.step();
    }
}

}  // namespace

// Command `comm` (0-7) with its vertices, local coordinates added.
static void draw_shape(const Cmd& c0, const uint8_t* cmd, int comm, Pt* p) {
    Cmd c = c0;
    if (c.textured && c.cmode == 1)             // the lookup table's address ignores the colour's low 2 bits
        for (int i = 0; i < 16; ++i) c.lut[i] = tw((uint32_t)(c.colr & ~3) * 8 + i * 2);
    if (c.textured && c.cmode == 5) c.srca &= ~0xFu;
    uint16_t gt[4] = {packed(c.g[0]), packed(c.g[1]), packed(c.g[2]), packed(c.g[3])};
    Vtx v[4];
    for (int i = 0; i < 4; ++i) v[i] = {p[i].x, p[i].y, gt[i], 0};
    switch (comm) {
    case 0x0: {                                 // normal sprite
        v[1] = {v[0].x + std::max(c.w, 1) - 1, v[0].y, gt[1], 0};
        v[2] = {v[1].x, v[0].y + std::max(c.h, 1) - 1, gt[2], 0};
        v[3] = {v[0].x, v[2].y, gt[3], 0};
        raster_quad(c, v, true);
        break;
    }
    case 0x1: {                                 // scaled sprite
        int zp = c.ctrl >> 8 & 0xF;
        int32_t x = sx13(cw(cmd, 12)) + g_local_x, y = sx13(cw(cmd, 14)) + g_local_y;
        int32_t w = sx13(cw(cmd, 16)), h = sx13(cw(cmd, 18));
        int32_t alt_x = sx13(cw(cmd, 20)) + g_local_x, alt_y = sx13(cw(cmd, 22)) + g_local_y;
        for (int i = 0; i < 4; ++i) { v[i].x = x; v[i].y = y; }
        switch (zp >> 2) {
        case 0: v[2].y = alt_y; v[3].y = alt_y; break;
        case 1: v[2].y += h; v[3].y += h; break;
        case 2: v[0].y -= h >> 1; v[1].y -= h >> 1; v[2].y += (h + 1) >> 1; v[3].y += (h + 1) >> 1; break;
        case 3: v[0].y -= h; v[1].y -= h; break;
        }
        switch (zp & 3) {
        case 0: v[1].x = alt_x; v[2].x = alt_x; break;
        case 1: v[1].x += w; v[2].x += w; break;
        case 2: v[0].x -= w >> 1; v[1].x += (w + 1) >> 1; v[2].x += (w + 1) >> 1; v[3].x -= w >> 1; break;
        case 3: v[0].x -= w; v[3].x -= w; break;
        }
        raster_quad(c, v, true);
        break;
    }
    case 0x2: case 0x3:                         // distorted sprite
        raster_quad(c, v, true);
        break;
    case 0x4:                                   // polygon
        raster_quad(c, v, false);
        break;
    case 0x5: case 0x7:                         // polyline
        for (int i = 0; i < 4; ++i) raster_line(c, v[i], v[(i + 1) & 3], false, 0, true);
        break;
    case 0x6:                                   // line
        raster_line(c, v[0], v[1], false, 0, true);
        break;
    }
}

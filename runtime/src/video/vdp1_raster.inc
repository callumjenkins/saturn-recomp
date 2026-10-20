// saturn-recomp runtime — VDP1's shapes drawn to pixels by saturn-recomp's own rules,
// included by vdp1.cpp. These rules come from Sega's manual alone. Mednafen's
// (vdp1_raster_gpl.inc) round edges, lines and texture coordinates differently, and the two put
// the edges of Saturn Bomberman's scaled title sprites up to a pixel apart.
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
// What a draw costs the chip, in g_draw_cycles, was fitted to the cycles Beetle Saturn (Mednafen's
// Saturn core) charged every command of Sonic Jam's classics, Sonic World and Saturn Bomberman's
// battle-round. Lines were not measured and cost as a polygon's lines.
// - a normal sprite costs each row 8 cycles when 8 dots wide, else 12, and 19/16 a dot. A row stops
//   where it leaves the clip window, but one that starts beyond it is walked in full;
// - a scaled sprite costs each row 13.3 and 1.1 a dot, for the dots inside the clip window only;
// - distorted sprites and polygons cost each line, and each dot up to where the line leaves the
//   clip window (all of it if it never enters): 9.8 and 1.35 a dot, or 7.6 and 1.6 for polygons.
//

static bool outside_clip(const Cmd& c, int x0, int y0, int x1, int y1);

static void line(const Cmd& c, Pt p0, Pt p1, int v, Rgb g0, Rgb g1) {
    if (outside_clip(c, std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x), std::max(p0.y, p1.y))) return;
    int dx = p1.x - p0.x, dy = p1.y - p0.y;
    int adx = std::abs(dx), ady = std::abs(dy);
    int len = std::max(adx, ady);
    g_draw_steps += (uint64_t)len + 1;
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
    if (x1 < 0 || y1 < 0 || x0 > std::min(g_sys_x, g_fb8 ? 1023 : 511) || y0 > std::min(g_sys_y, g_hi_target ? 511 : 255)) return true;
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
    for (int i = 0; i <= n && g_draw_steps < kDrawSteps; ++i) {
        Pt l{step_to(a.x, d.x, i, n), step_to(a.y, d.y, i, n)};
        Pt r{step_to(b.x, cc.x, i, n), step_to(b.y, cc.y, i, n)};
        int v = c.textured ? (int)((int64_t)i * c.h / (n + 1)) : 0;
        Rgb gl{}, gr{};
        if (c.gouraud) { gl = mix(c.g[0], c.g[3], i, n); gr = mix(c.g[1], c.g[2], i, n); }
        line(c, l, r, v, gl, gr);
    }
}

struct ClipBox { int x0, y0, x1, y1; };

static ClipBox clip_box(const Cmd& c) {
    ClipBox b{0, 0, g_sys_x, g_sys_y};
    if (c.clip && !c.outside) {
        b.x0 = std::max(b.x0, g_ux0); b.y0 = std::max(b.y0, g_uy0);
        b.x1 = std::min(b.x1, g_ux1); b.y1 = std::min(b.y1, g_uy1);
    }
    return b;
}

// The dots of the line a-b the chip steps through: up to where it leaves the box, or all of them if
// it never enters it.
static int dots_until_exit(Pt a, Pt b, const ClipBox& box) {
    int n = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
    bool inside = false;
    for (int i = 0; i <= n; ++i) {
        int x = step_to(a.x, b.x, i, n), y = step_to(a.y, b.y, i, n);
        if (x >= box.x0 && x <= box.x1 && y >= box.y0 && y <= box.y1) inside = true;
        else if (inside) return i;
    }
    return n + 1;
}

// The cycles of quad()'s lines from edge a-d to edge b-c.
static double quad_cycles(Pt a, Pt b, Pt cc, Pt d, const ClipBox& box, double per_line, double per_dot) {
    int n = std::max({std::abs(d.x - a.x), std::abs(d.y - a.y), std::abs(cc.x - b.x), std::abs(cc.y - b.y)});
    n = std::min(n, 2048);
    double cycles = 0;
    for (int i = 0; i <= n; ++i) {
        Pt l{step_to(a.x, d.x, i, n), step_to(a.y, d.y, i, n)};
        Pt r{step_to(b.x, cc.x, i, n), step_to(b.y, cc.y, i, n)};
        cycles += per_line + per_dot * dots_until_exit(l, r, box);
    }
    return cycles;
}

// Command `comm` (0-7) with its vertices: sprites and polygons as quadrilaterals, lines alone.
static void draw_shape(const Cmd& c, const uint8_t* cmd, int comm, Pt* p) {
    ClipBox box = clip_box(c);
    double cycles = 0;
    switch (comm) {
    case 0x0: {                                 // normal sprite
        int x0 = p[0].x, y0 = p[0].y, x1 = x0 + c.w - 1, y1 = y0 + c.h - 1;
        int cols = x0 <= box.x1 ? std::clamp(box.x1 - x0 + 1, 0, c.w) : c.w;
        cycles = c.h * ((c.w <= 8 ? 8 : 12) + cols * 19.0 / 16);
        quad(c, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1});
        break;
    }
    case 0x1: {                                 // scaled sprite
        int zp = c.ctrl >> 8 & 0xF;
        int x0, y0, x1, y1;
        if (!zp) {
            x0 = p[0].x; y0 = p[0].y; x1 = p[2].x; y1 = p[2].y;
        } else {
            int w = sx13(cw(cmd, 16)), h = sx13(cw(cmd, 18));   // B: the width and height on screen
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
        int vis_x = std::max(0, std::min(std::max(x0, x1), box.x1) - std::max(std::min(x0, x1), box.x0) + 1);
        int vis_y = std::max(0, std::min(std::max(y0, y1), box.y1) - std::max(std::min(y0, y1), box.y0) + 1);
        cycles = (std::abs(y1 - y0) + 1) * 13.3 + (double)vis_x * vis_y * 1.1;
        quad(c, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1});
        break;
    }
    case 0x2: case 0x3: case 0x4:              // distorted sprite, polygon
        cycles = comm == 0x4 ? quad_cycles(p[0], p[1], p[2], p[3], box, 7.6, 1.6)
                             : quad_cycles(p[0], p[1], p[2], p[3], box, 9.8, 1.35);
        quad(c, p[0], p[1], p[2], p[3]);
        break;
    case 0x5: case 0x7:                         // polyline
        for (int i = 0; i < 4; ++i) {
            cycles += 7.6 + 1.6 * dots_until_exit(p[i], p[(i + 1) & 3], box);
            line(c, p[i], p[(i + 1) & 3], 0, c.g[i], c.g[(i + 1) & 3]);
        }
        break;
    case 0x6:                                   // line
        cycles = 7.6 + 1.6 * dots_until_exit(p[0], p[1], box);
        line(c, p[0], p[1], 0, c.g[0], c.g[1]);
        break;
    }
    g_draw_cycles += (uint64_t)std::lround(cycles);
}

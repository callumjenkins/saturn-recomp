// saturn-recomp runtime — the host's drawing (ui.h).
#include "ui.h"
#include "inter_regular.h"
#include "inter_semibold.h"
#include <cmath>
#include <map>
#include <memory>
#include <vector>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace {

SDL_Renderer* g_ren;

// The characters baked: ASCII, then the few others the menu writes.
const int kAscii = 95;
const int kExtra[] = {0xB7, 0x2026, 0x2212, 0x2039, 0x203A, 0x2022, 0x2190, 0x2191, 0x2192, 0x2193};
const int kExtras = sizeof kExtra / sizeof kExtra[0];

struct Atlas {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    float px = 0;                    // the font size baked, in stb_truetype's pixel height
    float ascent = 0, descent = 0;   // at that size, in pixels: ascent above the baseline, descent (negative) below
    stbtt_packedchar ascii[kAscii], extra[kExtras];
};

// A size a pixel at a time up to 48, then every 4: few atlases, each sharp enough.
int bucket(float size) { return size <= 48 ? (int)std::lround(size) : (int)std::lround(size / 4) * 4; }

Atlas* atlas(float size, bool bold) {
    static std::map<std::pair<int, bool>, std::unique_ptr<Atlas>> cache;
    int px = std::max(6, bucket(size));
    auto& slot = cache[{px, bold}];
    if (slot) return slot.get();
    const unsigned char* font = bold ? inter_semibold : inter_regular;
    stbtt_fontinfo info;
    stbtt_InitFont(&info, font, stbtt_GetFontOffsetForIndex(font, 0));
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    // `size` is ascent to descent, which is what stbtt_ScaleForPixelHeight takes
    float scale = stbtt_ScaleForPixelHeight(&info, (float)px);
    auto a = std::make_unique<Atlas>();
    a->px = (float)px;
    a->ascent = asc * scale;
    a->descent = desc * scale;
    a->w = 512;
    a->h = 64;
    std::vector<unsigned char> alpha;
    for (;;) {
        alpha.assign((size_t)a->w * a->h, 0);
        stbtt_pack_context pc;
        stbtt_PackBegin(&pc, alpha.data(), a->w, a->h, 0, 1, nullptr);
        stbtt_PackSetOversampling(&pc, 2, 1);
        stbtt_pack_range ranges[2] = {};
        ranges[0].font_size = (float)px;
        ranges[0].first_unicode_codepoint_in_range = 32;
        ranges[0].num_chars = kAscii;
        ranges[0].chardata_for_range = a->ascii;
        ranges[1].font_size = (float)px;
        ranges[1].array_of_unicode_codepoints = const_cast<int*>(kExtra);
        ranges[1].num_chars = kExtras;
        ranges[1].chardata_for_range = a->extra;
        bool ok = stbtt_PackFontRanges(&pc, font, 0, ranges, 2);
        stbtt_PackEnd(&pc);
        if (ok) break;
        if (a->h < a->w) a->h *= 2;
        else a->w *= 2;
    }
    std::vector<uint32_t> rgba(alpha.size());
    for (size_t i = 0; i < alpha.size(); ++i) rgba[i] = 0x00FFFFFFu | ((uint32_t)alpha[i] << 24);
    a->tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, a->w, a->h);
    if (a->tex) {
        SDL_UpdateTexture(a->tex, nullptr, rgba.data(), a->w * 4);
        SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(a->tex, SDL_SCALEMODE_LINEAR);
    }
    slot = std::move(a);
    return slot.get();
}

// The next code point of UTF-8 `s` from `i`, which it moves past.
int next_code(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1, code = c & (0x3F >> n);
    for (; n > 0 && i < s.size(); --n) code = (code << 6) | ((unsigned char)s[i++] & 0x3F);
    return code;
}

const stbtt_packedchar* glyph(const Atlas& a, int code, int& index) {
    if (code >= 32 && code < 32 + kAscii) {
        index = code - 32;
        return a.ascii;
    }
    for (int k = 0; k < kExtras; ++k)
        if (kExtra[k] == code) {
            index = k;
            return a.extra;
        }
    index = '?' - 32;
    return a.ascii;
}

}  // namespace

void ui_open(SDL_Renderer* ren) { g_ren = ren; }

void ui_fill_rounded(SDL_FRect r, float rad, SDL_FColor c) {
    const int kArc = 8;
    rad = std::min(rad, std::min(r.w, r.h) / 2);
    std::vector<SDL_Vertex> v;
    v.push_back({{r.x + r.w / 2, r.y + r.h / 2}, c, {}});
    const float cx[] = {r.x + r.w - rad, r.x + rad, r.x + rad, r.x + r.w - rad};
    const float cy[] = {r.y + r.h - rad, r.y + r.h - rad, r.y + rad, r.y + rad};
    for (int q = 0; q < 4; ++q)
        for (int i = 0; i <= kArc; ++i) {
            float a = (q + (float)i / kArc) * SDL_PI_F / 2;
            v.push_back({{cx[q] + rad * std::cos(a), cy[q] + rad * std::sin(a)}, c, {}});
        }
    std::vector<int> idx;
    for (int i = 1; i + 1 < (int)v.size(); ++i) idx.insert(idx.end(), {0, i, i + 1});
    idx.insert(idx.end(), {0, (int)v.size() - 1, 1});
    SDL_RenderGeometry(g_ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

void ui_fill_circle(float x, float y, float r, SDL_FColor c) { ui_fill_rounded({x - r, y - r, 2 * r, 2 * r}, r, c); }

float ui_text_width(float size, const std::string& s, bool bold) {
    Atlas* a = atlas(size, bold);
    float k = size / a->px, w = 0;
    for (size_t i = 0; i < s.size();) {
        int index;
        const stbtt_packedchar* set = glyph(*a, next_code(s, i), index);
        w += set[index].xadvance;
    }
    return w * k;
}

void ui_text(float x, float y, float size, const std::string& s, SDL_FColor c, bool bold, UiAlign align) {
    Atlas* a = atlas(size, bold);
    if (!a->tex || s.empty()) return;
    float k = size / a->px;
    float w = ui_text_width(size, s, bold);
    float left = align == UiAlign::Left ? x : align == UiAlign::Right ? x - w : x - w / 2;
    // the baseline, so that ascent to descent is centred on y
    float base = y + (a->ascent + a->descent) / 2 * k;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    float pen = 0, dummy = 0;
    for (size_t i = 0; i < s.size();) {
        int index;
        const stbtt_packedchar* set = glyph(*a, next_code(s, i), index);
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(set, a->w, a->h, index, &pen, &dummy, &q, 0);
        float x0 = left + q.x0 * k, x1 = left + q.x1 * k, y0 = base + q.y0 * k, y1 = base + q.y1 * k;
        int n = (int)v.size();
        v.push_back({{x0, y0}, c, {q.s0, q.t0}});
        v.push_back({{x1, y0}, c, {q.s1, q.t0}});
        v.push_back({{x1, y1}, c, {q.s1, q.t1}});
        v.push_back({{x0, y1}, c, {q.s0, q.t1}});
        idx.insert(idx.end(), {n, n + 1, n + 2, n, n + 2, n + 3});
    }
    SDL_RenderGeometry(g_ren, a->tex, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

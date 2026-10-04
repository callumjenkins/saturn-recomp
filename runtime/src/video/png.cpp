// saturn-recomp runtime — a frame as a PNG file, with stored (uncompressed)
// deflate blocks: --shot and F12.
#include "saturn.h"
#include "video.h"
#include <algorithm>
#include <cstdio>
#include <vector>

static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

void write_png(const std::string& path, const Frame& fr) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(fr.w * 3 + 1) * fr.h);
    for (int y = 0; y < fr.h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < fr.w; ++x) {
            uint32_t p = fr.px[(size_t)y * fr.w + x];
            raw.push_back((uint8_t)(p >> 16));
            raw.push_back((uint8_t)(p >> 8));
            raw.push_back((uint8_t)p);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (ptrdiff_t)off, raw.begin() + (ptrdiff_t)(off + n));
        off += n;
    }
    uint32_t ad = b << 16 | a;
    for (int i = 3; i >= 0; --i) z.push_back((uint8_t)(ad >> (8 * i)));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { sat_note("cannot write %s", path.c_str()); return; }
    auto be = [&](uint32_t v) {
        uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        std::fwrite(t, 1, 4, f);
    };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
        be((uint32_t)d.size());
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), d.begin(), d.end());
        std::fwrite(td.data(), 1, td.size(), f);
        be(crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
    };
    std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::vector<uint8_t> ihdr = {(uint8_t)(fr.w >> 24), (uint8_t)(fr.w >> 16), (uint8_t)(fr.w >> 8), (uint8_t)fr.w,
                                 (uint8_t)(fr.h >> 24), (uint8_t)(fr.h >> 16), (uint8_t)(fr.h >> 8), (uint8_t)fr.h,
                                 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(f);
}

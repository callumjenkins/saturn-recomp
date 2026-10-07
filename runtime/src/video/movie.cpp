// saturn-recomp runtime — --video FILE: every field's picture and the run's sound
// as an MP4, through ffmpeg.
//
// Fields go to ffmpeg as raw RGB at 59.94 a second, on a canvas twice the first
// field's size; a field of another size is scaled to fit it and centred. The
// sound goes to a WAV beside the video (--wav's, or FILE.wav), and the two are
// joined when the run ends, a fatal error included.
#include "saturn.h"
#include "video.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

static FILE* g_pipe;
static int g_w, g_h;
static std::string g_video;                     // the picture alone, until the sound joins it
static std::vector<uint8_t> g_canvas;

static std::string quoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

bool movie_on() { return !g_cfg.video.empty(); }

std::string movie_wav() { return g_cfg.wav.empty() ? g_cfg.video + ".wav" : g_cfg.wav; }

void movie_frame(const Frame& f) {
    if (!movie_on() || f.w <= 0 || f.h <= 0) return;
    if (!g_pipe) {
        g_w = f.w * 2;
        g_h = f.h * 2;
        g_video = g_cfg.video + ".picture.mp4";
        std::string cmd = "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s " + std::to_string(g_w) + "x" +
                          std::to_string(g_h) + " -r 60000/1001 -i - -c:v libx264 -preset veryfast -crf 18 "
                          "-pix_fmt yuv420p " + quoted(g_video);
        g_pipe = popen(cmd.c_str(), "w");
        if (!g_pipe) sat_fatal("--video: cannot start ffmpeg");
        g_canvas.assign((size_t)g_w * g_h * 3, 0);
    }
    // nearest dot, the largest size that fits, centred
    double k = std::min((double)g_w / f.w, (double)g_h / f.h);
    int w = (int)(f.w * k), h = (int)(f.h * k), x0 = (g_w - w) / 2, y0 = (g_h - h) / 2;
    std::fill(g_canvas.begin(), g_canvas.end(), 0);
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = &f.px[(size_t)(int)(y / k) * f.w];
        uint8_t* out = &g_canvas[((size_t)(y0 + y) * g_w + x0) * 3];
        for (int x = 0; x < w; ++x) {
            uint32_t p = row[(int)(x / k)];
            *out++ = (uint8_t)(p >> 16);
            *out++ = (uint8_t)(p >> 8);
            *out++ = (uint8_t)p;
        }
    }
    std::fwrite(g_canvas.data(), 1, g_canvas.size(), g_pipe);
}

void movie_finish() {
    if (!g_pipe) return;
    pclose(g_pipe);
    g_pipe = nullptr;
    std::string cmd = "ffmpeg -y -loglevel error -i " + quoted(g_video) + " -i " + quoted(movie_wav()) +
                      " -c:v copy -c:a aac -b:a 192k -shortest " + quoted(g_cfg.video);
    if (std::system(cmd.c_str()) == 0) {
        std::remove(g_video.c_str());
        if (g_cfg.wav.empty()) std::remove(movie_wav().c_str());
    } else {
        std::fprintf(stderr, "  --video: joining the sound failed; the picture alone is %s\n", g_video.c_str());
    }
}

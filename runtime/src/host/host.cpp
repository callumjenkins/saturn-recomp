// saturn-recomp runtime — the host: a window (SDL3, OpenGL 4.5) that shows each
// field VDP2 composes, the pad from the keyboard and a gamepad, the pace
// and the audio stream.
//
// The frame is shown at VBlank-IN (video.cpp), scaled to a 4:3 picture as
// large as the window allows; for a PAL disc the screen is 256 lines tall,
// and a 224-line picture sits in the middle of it, as on a European TV. Time stays virtual (a run with the same pad
// is the same run); with a window, each VBlank-IN waits until the host's
// clock has caught up with it, so the game runs at the Saturn's speed, and
// a host that falls behind by more than 100 ms is not made to catch up.
// --realtime takes the host's clock itself instead.
//
// The pad (port 1, a standard Saturn pad; smpc.cpp):
//   keyboard  arrows, Enter START, Z X C = A B C, A S D = X Y Z, Q W = L R
//   gamepad   d-pad or left stick, Start, south A, east B, right shoulder C,
//             west X, north Y, left shoulder Z, triggers L and R
// F12 saves the picture (out/shot-VBLANK.png); F11 or Alt+Enter switches
// fullscreen. Closing the window ends the run.
//
// Sound (sound.cpp) comes as it is made, in virtual time, into an SDL audio
// stream at 44 100 Hz: the window's pace keeps it level with the device.
// The stream starts 50 ms ahead; a chunk that would take it past 250 ms is
// dropped (the host's clock and the device's drift apart), and one that
// finds it empty (the host fell behind) is preceded by 50 ms of silence.
#include "saturn.h"
#include "sound.h"
#include "video.h"
#include "host.h"
#include <SDL3/SDL.h>
#include <GL/glcorearb.h>
#include <algorithm>
#include <chrono>
#include <cstring>

#define HOST_GL_FUNCS(X)                                                            \
    X(PFNGLCLEARPROC, glClear)                                                      \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                            \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                  \
    X(PFNGLCREATETEXTURESPROC, glCreateTextures)                                    \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                    \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D)                                \
    X(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)                              \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                          \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers)                            \
    X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture)                  \
    X(PFNGLBLITNAMEDFRAMEBUFFERPROC, glBlitNamedFramebuffer)
#define HOST_GL_DEFINE(T, n) static T n;
HOST_GL_FUNCS(HOST_GL_DEFINE)
#undef HOST_GL_DEFINE

static SDL_Window* g_win;
static SDL_Gamepad* g_pad;
static GLuint g_tex, g_fbo;
static int g_tex_w, g_tex_h;
static uint16_t g_buttons;
static const Frame* g_last;
using Clock = std::chrono::steady_clock;
static Clock::time_point g_base;                // host time of virtual time 0
static bool g_paced;

bool host_wants_frame() { return g_win != nullptr; }
uint16_t host_pad() { return g_buttons; }

bool host_open() {
    if (g_cfg.headless) return true;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { sat_note("SDL: %s", SDL_GetError()); return false; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int s = std::max(1, g_cfg.scale);
    g_win = SDL_CreateWindow("saturn-recomp", 320 * s, 240 * s, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                             SDL_WINDOW_HIGH_PIXEL_DENSITY | (g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!g_win) { sat_note("SDL: %s", SDL_GetError()); return false; }
    if (!SDL_GL_CreateContext(g_win)) { sat_note("SDL: no OpenGL 4.5 context: %s", SDL_GetError()); return false; }
    bool ok = true;
#define HOST_GL_LOAD(T, n)                                                          \
    n = reinterpret_cast<T>(SDL_GL_GetProcAddress(#n));                             \
    if (!n) { sat_note("OpenGL: no %s", #n); ok = false; }
    HOST_GL_FUNCS(HOST_GL_LOAD)
#undef HOST_GL_LOAD
    if (!ok) return false;
    SDL_GL_SetSwapInterval(1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glCreateFramebuffers(1, &g_fbo);
    g_base = Clock::now();
    g_paced = !g_cfg.realtime;
    return true;
}

// marks the host call the machine's thread is in while it lasts
struct HostCall {
    explicit HostCall(const char* what) { g_host_call = what; }
    ~HostCall() { g_host_call = ""; }
};

// ---- sound ------------------------------------------------------------------------------------
static SDL_AudioStream* g_audio;
static const int kLead = 2205, kMax = 11025;    // frames: 50 ms ahead, 250 ms at most
static int g_underruns, g_dropped;

static void audio_silence(int frames) {
    std::vector<int16_t> z((size_t)frames * 2);
    SDL_PutAudioStreamData(g_audio, z.data(), (int)(z.size() * sizeof(int16_t)));
}

bool host_audio_open() {
    if (!g_win) return false;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_audio) { sat_note("SDL audio: %s (no sound)", SDL_GetError()); return false; }
    audio_silence(kLead);
    SDL_ResumeAudioStreamDevice(g_audio);
    return true;
}

void host_audio_push(const int16_t* lr, int frames) {
    int queued = [] { HostCall in("SDL_GetAudioStreamQueued"); return SDL_GetAudioStreamQueued(g_audio) / 4; }();
    if (queued + frames > kMax) { ++g_dropped; return; }
    if (queued == 0) { ++g_underruns; audio_silence(kLead); }
    HostCall in("SDL_PutAudioStreamData");
    SDL_PutAudioStreamData(g_audio, lr, frames * 4);
}

void host_audio_report() {
    if (g_audio) std::fprintf(stderr, "  audio stream: %d times empty, %d chunks dropped\n", g_underruns, g_dropped);
}

// ---- the pad ----------------------------------------------------------------------------------
enum : uint16_t {
    B_RIGHT = 0x8000, B_LEFT = 0x4000, B_DOWN = 0x2000, B_UP = 0x1000, B_START = 0x0800, B_A = 0x0400,
    B_C = 0x0200, B_B = 0x0100, B_R = 0x0080, B_X = 0x0040, B_Y = 0x0020, B_Z = 0x0010, B_L = 0x0008,
};

static void read_pad() {
    uint16_t b = 0;
    const bool* k = SDL_GetKeyboardState(nullptr);
    bool alt = SDL_GetModState() & SDL_KMOD_ALT;
    static const struct { SDL_Scancode key; uint16_t bit; } kKeys[] = {
        {SDL_SCANCODE_RIGHT, B_RIGHT}, {SDL_SCANCODE_LEFT, B_LEFT}, {SDL_SCANCODE_DOWN, B_DOWN},
        {SDL_SCANCODE_UP, B_UP}, {SDL_SCANCODE_RETURN, B_START}, {SDL_SCANCODE_Z, B_A},
        {SDL_SCANCODE_X, B_B}, {SDL_SCANCODE_C, B_C}, {SDL_SCANCODE_A, B_X}, {SDL_SCANCODE_S, B_Y},
        {SDL_SCANCODE_D, B_Z}, {SDL_SCANCODE_Q, B_L}, {SDL_SCANCODE_W, B_R}};
    for (auto& m : kKeys)
        if (k[m.key] && !(alt && m.key == SDL_SCANCODE_RETURN)) b |= m.bit;
    if (g_pad) {
        static const struct { SDL_GamepadButton btn; uint16_t bit; } kButtons[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP},
            {SDL_GAMEPAD_BUTTON_START, B_START}, {SDL_GAMEPAD_BUTTON_SOUTH, B_A},
            {SDL_GAMEPAD_BUTTON_EAST, B_B}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_C},
            {SDL_GAMEPAD_BUTTON_WEST, B_X}, {SDL_GAMEPAD_BUTTON_NORTH, B_Y},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_Z}};
        for (auto& m : kButtons)
            if (SDL_GetGamepadButton(g_pad, m.btn)) b |= m.bit;
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= B_L;
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= B_R;
        int x = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (x > 16000) b |= B_RIGHT;
        if (x < -16000) b |= B_LEFT;
        if (y > 16000) b |= B_DOWN;
        if (y < -16000) b |= B_UP;
    }
    if ((b & (B_LEFT | B_RIGHT)) == (B_LEFT | B_RIGHT)) b &= (uint16_t)~(B_LEFT | B_RIGHT);   // not on a real pad
    if ((b & (B_UP | B_DOWN)) == (B_UP | B_DOWN)) b &= (uint16_t)~(B_UP | B_DOWN);
    g_buttons = b;
}

static void save_shot() {
    if (!g_last) return;
    char name[64];
    std::snprintf(name, sizeof name, "/shot-%llu.png", (unsigned long long)sat_vblanks());
    write_png(g_cfg.out + name, *g_last);
    sat_note("picture saved: %s%s", g_cfg.out.c_str(), name);
}

static void events() {
    SDL_Event e;
    HostCall in("SDL_PollEvent");
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            sat_stop("the window was closed");
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!g_pad) {
                g_pad = SDL_OpenGamepad(e.gdevice.which);
                if (g_pad) sat_note("gamepad: %s", SDL_GetGamepadName(g_pad));
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which) { SDL_CloseGamepad(g_pad); g_pad = nullptr; }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.scancode == SDL_SCANCODE_F12) save_shot();
            if (e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
            break;
        }
    }
    read_pad();
}

void host_present(const Frame& f) {
    if (!g_win) return;
    g_last = &f;
    if (f.w != g_tex_w || f.h != g_tex_h) {
        if (g_tex) glDeleteTextures(1, &g_tex);
        glCreateTextures(GL_TEXTURE_2D, 1, &g_tex);
        glTextureStorage2D(g_tex, 1, GL_RGBA8, f.w, f.h);
        glNamedFramebufferTexture(g_fbo, GL_COLOR_ATTACHMENT0, g_tex, 0);
        g_tex_w = f.w;
        g_tex_h = f.h;
    }
    glTextureSubImage2D(g_tex, 0, 0, 0, f.w, f.h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, f.px.data());
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // the TV's 4:3 screen, as large as the window allows, centred; a PAL TV
    // shows 256 lines, and a 224-line picture has a border above and below
    int sw = ww, sh = ww * 3 / 4;
    if (sh > wh) { sh = wh; sw = wh * 4 / 3; }
    int tv_lines = bios_pal() ? std::max(256, f.h) : f.h;
    int ph = sh * f.h / tv_lines;
    int dx = (ww - sw) / 2, dy = (wh - ph) / 2;
    glBlitNamedFramebuffer(g_fbo, 0, 0, 0, f.w, f.h, dx, dy + ph, dx + sw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    {
        HostCall in("SDL_GL_SwapWindow");
        SDL_GL_SwapWindow(g_win);
    }
    events();
}

void host_pace(uint64_t now) {
    if (!g_win || !g_paced) return;
    Clock::time_point due = g_base + std::chrono::nanoseconds(now);
    Clock::time_point t = Clock::now();
    if (t > due + std::chrono::milliseconds(100)) g_base += t - due;   // behind: do not run to catch up
    else if (due > t) {
        HostCall in("SDL_DelayPrecise");
        SDL_DelayPrecise((Uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(due - t).count());
    }
}

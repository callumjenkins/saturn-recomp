// saturn-recomp runtime — the host: a window (SDL3, OpenGL 3.2) that shows each
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
// The pads (standard Saturn pads; smpc.cpp numbers the players across the ports):
//   keyboard  player 1: arrows, Enter START, Z X C = A B C, A S D = X Y Z, Q W = L R
//   gamepads  a player each, in the order they connect (pad_slots.h), the first
//             player 1 beside the keyboard: d-pad or left stick, Start, south A,
//             east B, right shoulder C, west X, north Y, left shoulder Z, triggers L and R
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
#include "pad_slots.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>               // GL's types and constants; its functions come from SDL_GL_GetProcAddress
#include <algorithm>
#include <chrono>
#include <cstring>

// OpenGL 3.2 core, the newest macOS gives; GL 1.1's names are declared by the header, hence the prefix
#define HOST_GL_FUNCS(X)                                                                              \
    X(void, Clear, (GLbitfield))                                                                       \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                          \
    X(void, PixelStorei, (GLenum, GLint))                                                              \
    X(void, GenTextures, (GLsizei, GLuint*))                                                           \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                  \
    X(void, BindTexture, (GLenum, GLuint))                                                             \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))  \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                       \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                         \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                             \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
#define HOST_GL_DEFINE(R, n, args) static R(*host_gl##n) args;
HOST_GL_FUNCS(HOST_GL_DEFINE)
#undef HOST_GL_DEFINE

static SDL_Window* g_win;
struct OpenPad { SDL_Gamepad* pad; SDL_JoystickID id; };
static std::vector<OpenPad> g_pads;
static PadSlots g_slots(kHostSlots);
static GLuint g_tex, g_fbo;
static int g_tex_w, g_tex_h;
static uint16_t g_buttons[kHostSlots];
static const Frame* g_last;
using Clock = std::chrono::steady_clock;
static Clock::time_point g_base;                // host time of virtual time 0
static bool g_paced;

bool host_wants_frame() { return g_win != nullptr; }
uint16_t host_pad(int slot) { return g_buttons[slot]; }
bool host_pad_connected(int slot) { return g_win && (slot == 0 || g_slots.taken(slot)); }

static void virtual_open();

bool host_open() {
    if (g_cfg.headless) return true;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { sat_note("SDL: %s", SDL_GetError()); return false; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);   // macOS needs it for a core context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int s = std::max(1, g_cfg.scale);
    g_win = SDL_CreateWindow("saturn-recomp", 320 * s, 240 * s, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                             SDL_WINDOW_HIGH_PIXEL_DENSITY | (g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!g_win) { sat_note("SDL: %s", SDL_GetError()); return false; }
    if (!SDL_GL_CreateContext(g_win)) { sat_note("SDL: no OpenGL 3.2 context: %s", SDL_GetError()); return false; }
    bool ok = true;
#define HOST_GL_LOAD(R, n, args)                                                    \
    host_gl##n = reinterpret_cast<R(*) args>(SDL_GL_GetProcAddress("gl" #n));       \
    if (!host_gl##n) { sat_note("OpenGL: no gl%s", #n); ok = false; }
    HOST_GL_FUNCS(HOST_GL_LOAD)
#undef HOST_GL_LOAD
    if (!ok) return false;
    // host_pace keeps real time; a swap that waited for the display would stall the whole machine
    // whenever the compositor stops sending frames, as KWin does for a window that is out of sight
    SDL_GL_SetSwapInterval(0);
    if (!g_cfg.virtual_input.empty()) virtual_open();
    host_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    host_glGenFramebuffers(1, &g_fbo);
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

static uint16_t gamepad_buttons(SDL_Gamepad* pad) {
    static const struct { SDL_GamepadButton btn; uint16_t bit; } kButtons[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP},
        {SDL_GAMEPAD_BUTTON_START, B_START}, {SDL_GAMEPAD_BUTTON_SOUTH, B_A},
        {SDL_GAMEPAD_BUTTON_EAST, B_B}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_C},
        {SDL_GAMEPAD_BUTTON_WEST, B_X}, {SDL_GAMEPAD_BUTTON_NORTH, B_Y},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_Z}};
    uint16_t b = 0;
    for (auto& m : kButtons)
        if (SDL_GetGamepadButton(pad, m.btn)) b |= m.bit;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= B_L;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= B_R;
    int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (x > 16000) b |= B_RIGHT;
    if (x < -16000) b |= B_LEFT;
    if (y > 16000) b |= B_DOWN;
    if (y < -16000) b |= B_UP;
    return b;
}

static void read_pads() {
    uint16_t b[kHostSlots] = {};
    const bool* k = SDL_GetKeyboardState(nullptr);
    bool alt = SDL_GetModState() & SDL_KMOD_ALT;
    static const struct { SDL_Scancode key; uint16_t bit; } kKeys[] = {
        {SDL_SCANCODE_RIGHT, B_RIGHT}, {SDL_SCANCODE_LEFT, B_LEFT}, {SDL_SCANCODE_DOWN, B_DOWN},
        {SDL_SCANCODE_UP, B_UP}, {SDL_SCANCODE_RETURN, B_START}, {SDL_SCANCODE_Z, B_A},
        {SDL_SCANCODE_X, B_B}, {SDL_SCANCODE_C, B_C}, {SDL_SCANCODE_A, B_X}, {SDL_SCANCODE_S, B_Y},
        {SDL_SCANCODE_D, B_Z}, {SDL_SCANCODE_Q, B_L}, {SDL_SCANCODE_W, B_R}};
    for (auto& m : kKeys)
        if (k[m.key] && !(alt && m.key == SDL_SCANCODE_RETURN)) b[0] |= m.bit;
    for (auto& p : g_pads) {
        int s = g_slots.slot_of(p.id);
        if (s >= 0) b[s] |= gamepad_buttons(p.pad);
    }
    for (int s = 0; s < kHostSlots; ++s) {
        if ((b[s] & (B_LEFT | B_RIGHT)) == (B_LEFT | B_RIGHT)) b[s] &= (uint16_t)~(B_LEFT | B_RIGHT);   // not on a real pad
        if ((b[s] & (B_UP | B_DOWN)) == (B_UP | B_DOWN)) b[s] &= (uint16_t)~(B_UP | B_DOWN);
        g_buttons[s] = b[s];
    }
}

static void pad_added(SDL_JoystickID id) {
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) return;
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof guid);
    int s = g_slots.connect(id, guid);
    if (s < 0) {
        sat_note("gamepad: %s, not used: every player has one", SDL_GetGamepadName(pad));
        SDL_CloseGamepad(pad);
        return;
    }
    g_pads.push_back({pad, id});
    sat_note("gamepad: %s, player %d", SDL_GetGamepadName(pad), s + 1);
}

static void pad_removed(SDL_JoystickID id) {
    for (size_t i = 0; i < g_pads.size(); ++i)
        if (g_pads[i].id == id) {
            int s = g_slots.disconnect(id);
            sat_note("gamepad: %s gone, player %d free", SDL_GetGamepadName(g_pads[i].pad), s + 1);
            SDL_CloseGamepad(g_pads[i].pad);
            g_pads.erase(g_pads.begin() + (std::ptrdiff_t)i);
            return;
        }
}

static void save_shot() {
    if (!g_last) return;
    char name[64];
    std::snprintf(name, sizeof name, "/shot-%llu.png", (unsigned long long)sat_vblanks());
    write_png(g_cfg.out + name, *g_last);
    sat_note("picture saved: %s%s", g_cfg.out.c_str(), name);
}

// --virtual-input: SDL virtual gamepads, plugged in a player each in order, pressed by a script, so a
// test reaches the game through SDL and the slots as a player's controllers do
static std::vector<SDL_Joystick*> g_virtual;
static std::vector<PadStep> g_virtual_steps;
static size_t g_virtual_pos;

static void virtual_open() {
    g_virtual_steps = smpc_parse_script(g_cfg.virtual_input);
    int n = 0;
    for (auto& s : g_virtual_steps) n = std::max(n, s.pad + 1);
    for (int k = 0; k < n; ++k) {
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.name = "saturn-recomp virtual pad";
        SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
        if (!id) sat_fatal("--virtual-input: %s", SDL_GetError());
        SDL_Joystick* j = SDL_OpenJoystick(id);
        SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        g_virtual.push_back(j);
    }
}

static void virtual_press() {
    static const struct { uint16_t bit; SDL_GamepadButton btn; } kButtons[] = {
        {B_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {B_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {B_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {B_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {B_START, SDL_GAMEPAD_BUTTON_START}, {B_A, SDL_GAMEPAD_BUTTON_SOUTH}, {B_B, SDL_GAMEPAD_BUTTON_EAST},
        {B_C, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {B_X, SDL_GAMEPAD_BUTTON_WEST},
        {B_Y, SDL_GAMEPAD_BUTTON_NORTH}, {B_Z, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}};
    for (; g_virtual_pos < g_virtual_steps.size() && g_virtual_steps[g_virtual_pos].vblank <= sat_vblanks(); ++g_virtual_pos) {
        const PadStep& s = g_virtual_steps[g_virtual_pos];
        SDL_Joystick* j = g_virtual[s.pad];
        for (auto& m : kButtons) SDL_SetJoystickVirtualButton(j, m.btn, (s.pressed & m.bit) != 0);
        // a trigger at rest is the axis's minimum: 0 is half pulled
        SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, s.pressed & B_L ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
        SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, s.pressed & B_R ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    }
}

static void events() {
    if (!g_virtual.empty()) virtual_press();
    SDL_Event e;
    HostCall in("SDL_PollEvent");
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            sat_stop("the window was closed");
        case SDL_EVENT_GAMEPAD_ADDED:
            pad_added(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            pad_removed(e.gdevice.which);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.scancode == SDL_SCANCODE_F12) save_shot();
            if (e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
            break;
        }
    }
    read_pads();
}

void host_present(const Frame& f) {
    if (!g_win) return;
    g_last = &f;
    if (SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) {
        events();
        return;
    }
    if (f.w != g_tex_w || f.h != g_tex_h) {
        if (g_tex) host_glDeleteTextures(1, &g_tex);
        host_glGenTextures(1, &g_tex);
        host_glBindTexture(GL_TEXTURE_2D, g_tex);
        host_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.w, f.h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
        host_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        host_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_tex, 0);
        g_tex_w = f.w;
        g_tex_h = f.h;
    }
    host_glBindTexture(GL_TEXTURE_2D, g_tex);
    host_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.w, f.h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, f.px.data());
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    host_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    host_glClearColor(0, 0, 0, 1);
    host_glClear(GL_COLOR_BUFFER_BIT);
    // the TV's 4:3 screen, as large as the window allows, centred; a PAL TV
    // shows 256 lines, and a 224-line picture has a border above and below
    int sw = ww, sh = ww * 3 / 4;
    if (sh > wh) { sh = wh; sw = wh * 4 / 3; }
    int tv_lines = bios_pal() ? std::max(256, f.h) : f.h;
    int ph = sh * f.h / tv_lines;
    int dx = (ww - sw) / 2, dy = (wh - ph) / 2;
    host_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    host_glBlitFramebuffer(0, 0, f.w, f.h, dx, dy + ph, dx + sw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
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

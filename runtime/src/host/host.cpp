// saturn-recomp runtime — the host: a window (SDL3's renderer) that shows each
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
// unless the settings (settings.h) bind them otherwise.
// F12 saves the picture (out/shot-VBLANK.png); F11 or Alt+Enter switches
// fullscreen. Closing the window, or Android's Back, ends the run. Until --resume's VBlank the run
// goes as fast as it can, without sound, every 30th picture shown with a bar for how far it has got.
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
#include "settings.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstring>

static SDL_Window* g_win;
static PadSlots g_slots(kHostSlots);
static SDL_Renderer* g_ren;
static SDL_Texture* g_tex;
static int g_tex_w, g_tex_h;
static uint16_t g_buttons[kHostSlots];
static const Frame* g_last;
using Clock = std::chrono::steady_clock;
static Clock::time_point g_base;                // host time of virtual time 0
static bool g_paced;

bool host_wants_frame() { return g_win && (!sat_resuming() || sat_vblanks() % 30 == 0); }
uint16_t host_pad(int slot) { return g_buttons[slot]; }
bool host_pad_connected(int slot) { return g_win && (slot == 0 || g_slots.taken(slot)); }

static void virtual_open();

// ---- the settings (settings.h) -----------------------------------------------------------------
static Settings g_settings;
static void bind_keyboard();

static void load_settings() {
    std::string path = g_cfg.settings;
    if (path == "-") path.clear();
    else if (path.empty() && !sat_data_dir().empty()) path = sat_data_dir() + "/saturn-recomp/settings.ini";
    if (!path.empty()) {
        bool found = false;
        std::string text = settings_load(path, found);
        if (!found) {
            if (settings_store(path, Settings::default_text())) sat_note("settings: %s, written with the defaults", path.c_str());
            else sat_note("settings: cannot write %s (the defaults)", path.c_str());
        } else {
            std::vector<std::string> problems;
            g_settings.read(text, problems);
            sat_note("settings: %s", path.c_str());
            for (auto& p : problems) sat_note("%s, so its default", p.c_str());
        }
    }
    bind_keyboard();
}

bool host_open() {
    if (g_cfg.headless) return true;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { sat_note("SDL: %s", SDL_GetError()); return false; }
    load_settings();
    int s = g_cfg.scale > 0 ? g_cfg.scale : g_settings.number("display", "scale");
    bool full = g_cfg.fullscreen || g_settings.flag("display", "fullscreen");
#if defined(__ANDROID__)
    // SDL picks the phone's orientation itself, from the window, over the manifest's
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    full = true;
#endif
    if (!SDL_CreateWindowAndRenderer("saturn-recomp", 320 * s, 240 * s, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                     (full ? SDL_WINDOW_FULLSCREEN : 0), &g_win, &g_ren)) {
        sat_note("SDL: %s", SDL_GetError());
        return false;
    }
    sat_note("SDL: drawing with %s", SDL_GetRendererName(g_ren));
    // host_pace keeps real time; a present that waited for the display would stall the whole machine
    // whenever the compositor stops sending frames, as KWin does for a window that is out of sight
    SDL_SetRenderVSync(g_ren, 0);
    if (!g_cfg.virtual_input.empty()) virtual_open();
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
    SDL_SetAudioStreamGain(g_audio, g_settings.number("audio", "volume") / 100.0f);
    SDL_ResumeAudioStreamDevice(g_audio);
    return true;
}

void host_audio_push(const int16_t* lr, int frames) {
    if (sat_resuming()) return;
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

// The Saturn's buttons by the settings' names for them.
static const struct { const char* name; uint16_t bit; } kSaturn[] = {
    {"up", B_UP}, {"down", B_DOWN}, {"left", B_LEFT}, {"right", B_RIGHT}, {"start", B_START}, {"a", B_A},
    {"b", B_B}, {"c", B_C}, {"x", B_X}, {"y", B_Y}, {"z", B_Z}, {"l", B_L}, {"r", B_R}};

// The names in a setting's comma-separated list that `known` takes, or, if one is not, the default's.
template <typename F>
static std::vector<int> names(const Settings& set, const std::string& section, const std::string& key, F known) {
    std::vector<int> out;
    std::string list = set.get(section, key);
    for (size_t i = 0; i <= list.size();) {
        size_t j = std::min(list.find(',', i), list.size());
        std::string n = list.substr(i, j - i);
        n.erase(0, n.find_first_not_of(' '));
        n.erase(n.find_last_not_of(' ') + 1);
        i = j + 1;
        if (n.empty()) continue;
        int v = known(n);
        if (v < 0) {
            sat_note("settings: [%s] %s: no %s, so its default", section.c_str(), key.c_str(), n.c_str());
            return names(Settings(), Settings::kind_of(section), key, known);
        }
        out.push_back(v);
    }
    return out;
}

static std::vector<std::pair<SDL_Scancode, uint16_t>> g_keys;

static void bind_keyboard() {
    g_keys.clear();
    for (auto& b : kSaturn)
        for (int k : names(g_settings, "keyboard", b.name, [](const std::string& n) {
                 SDL_Scancode c = SDL_GetScancodeFromName(n.c_str());
                 return c == SDL_SCANCODE_UNKNOWN ? -1 : (int)c;
             }))
            g_keys.push_back({(SDL_Scancode)k, b.bit});
}

// A gamepad's bindings: its buttons and trigger axes, with the stick's and the triggers' travel.
struct PadInput { int button, axis; uint16_t bit; };
struct PadMap { std::vector<PadInput> inputs; int stick, trigger; };
struct OpenPad { SDL_Gamepad* pad; SDL_JoystickID id; PadMap map; };
static std::vector<OpenPad> g_pads;

static PadMap bind_gamepad(const std::string& guid) {
    std::string section = g_settings.has_section("gamepad " + guid) ? "gamepad " + guid : "gamepad";
    PadMap m;
    for (auto& b : kSaturn)
        for (int v : names(g_settings, section, b.name, [](const std::string& n) {
                 if (n == "lefttrigger") return 1000 + (int)SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
                 if (n == "righttrigger") return 1000 + (int)SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                 SDL_GamepadButton btn = SDL_GetGamepadButtonFromString(n.c_str());
                 return btn == SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)btn;
             }))
            m.inputs.push_back(v >= 1000 ? PadInput{-1, v - 1000, b.bit} : PadInput{v, -1, b.bit});
    m.stick = SDL_JOYSTICK_AXIS_MAX * g_settings.number(section, "stick") / 100;
    m.trigger = SDL_JOYSTICK_AXIS_MAX * g_settings.number(section, "trigger") / 100;
    return m;
}

static uint16_t gamepad_buttons(SDL_Gamepad* pad, const PadMap& m) {
    uint16_t b = 0;
    for (auto& in : m.inputs)
        if (in.button >= 0 ? SDL_GetGamepadButton(pad, (SDL_GamepadButton)in.button)
                           : SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)in.axis) > m.trigger)
            b |= in.bit;
    int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (x > m.stick) b |= B_RIGHT;
    if (x < -m.stick) b |= B_LEFT;
    if (y > m.stick) b |= B_DOWN;
    if (y < -m.stick) b |= B_UP;
    return b;
}

static void read_pads() {
    uint16_t b[kHostSlots] = {};
    const bool* k = SDL_GetKeyboardState(nullptr);
    bool alt = SDL_GetModState() & SDL_KMOD_ALT;
    for (auto& [key, bit] : g_keys)
        if (k[key] && !(alt && key == SDL_SCANCODE_RETURN)) b[0] |= bit;   // Alt+Enter is fullscreen
    for (auto& p : g_pads) {
        int s = g_slots.slot_of(p.id);
        if (s >= 0) b[s] |= gamepad_buttons(p.pad, p.map);
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
    g_pads.push_back({pad, id, bind_gamepad(guid)});
    sat_note("gamepad: %s (GUID %s), player %d", SDL_GetGamepadName(pad), guid, s + 1);
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
            state_stop("the window was closed");
        case SDL_EVENT_GAMEPAD_ADDED:
            pad_added(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            pad_removed(e.gdevice.which);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.scancode == SDL_SCANCODE_AC_BACK) state_stop("Back was pressed");     // Android's Back
            if (e.key.scancode == SDL_SCANCODE_F12) save_shot();
            if (e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))
                SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
            break;
        }
    }
    read_pads();
}

static void present(const Frame& f) {
    g_last = &f;
    if (SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) {
        events();
        return;
    }
    if (f.w != g_tex_w || f.h != g_tex_h) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, f.w, f.h);
        if (!g_tex) sat_fatal("SDL: no %dx%d texture: %s", f.w, f.h, SDL_GetError());
        SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_LINEAR);
        g_tex_w = f.w;
        g_tex_h = f.h;
    }
    SDL_UpdateTexture(g_tex, nullptr, f.px.data(), f.w * 4);
    int ww = 0, wh = 0;
    SDL_GetCurrentRenderOutputSize(g_ren, &ww, &wh);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    // the TV's 4:3 screen, as large as the window allows, centred; a PAL TV
    // shows 256 lines, and a 224-line picture has a border above and below
    int sw = ww, sh = ww * 3 / 4;
    if (sh > wh) { sh = wh; sw = wh * 4 / 3; }
    int tv_lines = bios_pal() ? std::max(256, f.h) : f.h;
    int ph = sh * f.h / tv_lines;
    SDL_FRect dst = {(float)((ww - sw) / 2), (float)((wh - ph) / 2), (float)sw, (float)ph};
    SDL_RenderTexture(g_ren, g_tex, nullptr, &dst);
    if (sat_resuming()) {                       // how far --resume has got
        SDL_FRect bar = {dst.x, dst.y + dst.h - 8, dst.w * sat_vblanks() / g_cfg.resume, 8};
        SDL_SetRenderDrawColor(g_ren, 255, 255, 255, 255);
        SDL_RenderFillRect(g_ren, &bar);
    }
    {
        HostCall in("SDL_RenderPresent");
        SDL_RenderPresent(g_ren);
    }
    events();
}

void host_present(const Frame& f) {
    if (!g_win) return;
    tasks_on_thread_stack([](void* f) { present(*static_cast<const Frame*>(f)); }, const_cast<Frame*>(&f));
}

void host_pace(uint64_t now) {
    if (!g_win || !g_paced) return;
    if (sat_resuming()) {
        g_base = Clock::now() - std::chrono::nanoseconds(now);
        return;
    }
    Clock::time_point due = g_base + std::chrono::nanoseconds(now);
    Clock::time_point t = Clock::now();
    if (t > due + std::chrono::milliseconds(100)) g_base += t - due;   // behind: do not run to catch up
    else if (due > t) {
        HostCall in("SDL_DelayPrecise");
        SDL_DelayPrecise((Uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(due - t).count());
    }
}

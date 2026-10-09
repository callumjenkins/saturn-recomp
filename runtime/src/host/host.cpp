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
// The touchscreen has a Saturn pad of its own for player 1 (touch_pad.h). Android's Back, Escape, a
// gamepad's Back or Guide, or the menu button at the top of a touchscreen opens a menu that pauses
// the game, sets the touch pad, and quits. F12 saves the picture (out/shot-VBLANK.png); F11 or Alt+Enter switches
// fullscreen. Closing the window, or Quit in the menu, ends the run. Until --resume's VBlank the run
// goes as fast as it can, without sound, with a card in place of the picture for how far it has got.
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
#include "touch_pad.h"
#include "ui.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

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
static bool g_touchscreen;                      // a touchscreen is there, or a finger has been seen

// ---- the settings (settings.h) -----------------------------------------------------------------
static Settings g_settings;
static std::string g_settings_path;
static void bind_keyboard();

static void load_settings() {
    std::string path = g_cfg.settings;
    if (path == "-") path.clear();
    else if (path.empty() && !sat_data_dir().empty()) path = sat_data_dir() + "/saturn-recomp/settings.ini";
    g_settings_path = path;
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
    ui_open(g_ren);
    // host_pace keeps real time; a present that waited for the display would stall the whole machine
    // whenever the compositor stops sending frames, as KWin does for a window that is out of sight
    SDL_SetRenderVSync(g_ren, 0);
    if (!g_cfg.virtual_input.empty()) virtual_open();
    // Android stops SDL's event pump inside SDL_PollEvent while the app is in the background, before
    // the events reach the run, so a watch sees them as they are sent
    SDL_AddEventWatch([](void*, SDL_Event* e) {
        if (e->type == SDL_EVENT_DID_ENTER_BACKGROUND) ++g_host_paused;
        if (e->type == SDL_EVENT_WILL_ENTER_FOREGROUND) --g_host_paused;
        return true;
    }, nullptr);
    int touch_devices = 0;
    SDL_free(SDL_GetTouchDevices(&touch_devices));
    g_touchscreen = touch_devices > 0;
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

static uint16_t g_held_over[kHostSlots];        // held as the menu closed: ignored until let go
static uint16_t touch_read();

static void read_pads() {
    uint16_t b[kHostSlots] = {};
    b[0] |= touch_read();
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
        g_held_over[s] &= b[s];
        g_buttons[s] = b[s] & (uint16_t)~g_held_over[s];
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

// ---- the touchscreen's pad (touch_pad.h) ------------------------------------------------------
static std::map<SDL_FingerID, TouchFinger> g_fingers;
// Fingers lifted before the pads were read since they went down: each holds its button for that one read,
// so a tap quicker than a frame still presses it.
static std::vector<SDL_FingerID> g_lifted;
static std::map<SDL_FingerID, bool> g_unread;
static uint16_t g_touch_held;

static int g_out_w, g_out_h;                    // the window's size in pixels, as last drawn

static bool touch_shown() {
    std::string mode = g_settings.get("touch", "controls");
    if (mode == "on") return true;
    if (mode == "off") return false;
    return g_touchscreen && g_pads.empty();
}

static bool menu_button_shown() { return g_touchscreen && g_settings.flag("touch", "menu_button"); }

// The window's pixels to a density-independent pixel: Android's density, 1 on most desktops.
static float display_scale() {
    float s = g_win ? SDL_GetWindowDisplayScale(g_win) : 0;
    return s > 0 ? s : 1;
}

// The pad's height on the screen: 480 density-independent pixels at its default size.
static TouchLayout pad_layout() {
    return touch_layout(g_out_w, g_out_h, 4.8f * display_scale() * (float)g_settings.number("touch", "size"));
}

static uint16_t touch_read() {
    std::vector<TouchFinger> fingers;
    for (auto& [id, f] : g_fingers) fingers.push_back(f);
    for (SDL_FingerID id : g_lifted) g_fingers.erase(id);
    g_lifted.clear();
    g_unread.clear();
    if (!touch_shown() || fingers.empty()) return g_touch_held = 0;
    return g_touch_held = touch_buttons(pad_layout(), fingers);
}

// ---- the menu ---------------------------------------------------------------------------------
// It pauses the game: virtual time stands still while it is open, and the host's clock is moved on
// by as long as it was, so the run is the same run with or without it. A panel of two columns where
// the screen is wide enough, the game's choices in one and the touch pad's in the other, each a row:
// an action, a choice of a few, a slider, or a switch.
enum MenuId { M_RESUME, M_SAVES, M_QUIT, M_PAD, M_SIZE, M_STYLE, M_MENU_BUTTON };
enum class Control { Action, Choice, Slider, Switch };
struct MenuRow { MenuId id; Control control; int column; SDL_FRect r; };
struct MenuLayout { SDL_FRect panel; float u; float title_y; float section_y[2]; float column_x[2]; std::vector<MenuRow> rows; };

static bool g_menu_open, g_quitting;
static int g_menu_at;                           // the focused row, by index into the layout's rows
static SDL_FingerID g_slider_finger;            // the finger moving the size slider, or 0
static const char* const kPadModes[] = {"auto", "on", "off"};
static const char* const kPadModeNames[] = {"Auto", "On", "Off"};
static const char* const kStyles[] = {"western", "japanese"};
static const char* const kStyleNames[] = {"Western", "Japanese"};

static void setting_change(const char* key, const char* value) {
    g_settings.set("touch", key, value);
    if (g_settings_path.empty()) return;
    bool found = false;
    std::string text = settings_load(g_settings_path, found);
    if (!settings_store(g_settings_path, settings_with(found ? text : Settings::default_text(), "touch", key, value)))
        sat_note("settings: cannot write %s", g_settings_path.c_str());
}

static int choice_count(MenuId id) { return id == M_PAD ? 3 : 2; }
static const char* choice_name(MenuId id, int i) { return id == M_PAD ? kPadModeNames[i] : kStyleNames[i]; }

static int choice_at(MenuId id) {
    std::string v = g_settings.get("touch", id == M_PAD ? "controls" : "style");
    for (int i = 0; i < choice_count(id); ++i)
        if (v == (id == M_PAD ? kPadModes[i] : kStyles[i])) return i;
    return 0;
}

static void choose(MenuId id, int i) {
    i = (i + choice_count(id)) % choice_count(id);
    setting_change(id == M_PAD ? "controls" : "style", id == M_PAD ? kPadModes[i] : kStyles[i]);
}

// The pad's size in percent, from half to twice its default, a tenth at a time.
static void pad_size_set(int size) {
    size = std::clamp((size + 5) / 10 * 10, 50, 200);
    setting_change("size", std::to_string(size).c_str());
}

static const char* menu_label(MenuId id) {
    switch (id) {
    case M_RESUME: return "Resume";
    case M_SAVES: return "Save files";
    case M_QUIT: return "Quit";
    case M_PAD: return "Show pad";
    case M_SIZE: return "Size";
    case M_STYLE: return "Style";
    default: return "Menu button";
    }
}

static MenuLayout menu_layout() {
    const float pad = 20, title = 56, section = 30, row = 52, gap = 6, column = 340, between = 24;
    std::vector<std::pair<MenuId, Control>> game = {{M_RESUME, Control::Action}};
    if (!g_cfg.after.empty()) game.push_back({M_SAVES, Control::Action});
    game.push_back({M_QUIT, Control::Action});
    const std::vector<std::pair<MenuId, Control>> touch = {
        {M_PAD, Control::Choice}, {M_SIZE, Control::Slider}, {M_STYLE, Control::Choice}, {M_MENU_BUTTON, Control::Switch}};
    float tall = (float)std::max(game.size(), touch.size());
    // two columns, or one, whichever is drawn bigger; never bigger than density-independent pixels make it
    float dp = display_scale(), w = 0.94f * g_out_w, h = 0.94f * g_out_h;
    float w2 = 2 * pad + 2 * column + between, h2 = 2 * pad + title + section + tall * (row + gap);
    float w1 = 2 * pad + column, h1 = 2 * pad + title + 2 * section + (float)(game.size() + touch.size()) * (row + gap) + gap;
    float k2 = std::min({dp, w / w2, h / h2}), k1 = std::min({dp, w / w1, h / h1});
    bool two = k2 >= k1;
    float u = two ? k2 : k1;
    MenuLayout l;
    l.u = u;
    float pw = (two ? w2 : w1) * u, ph = (two ? h2 : h1) * u;
    l.panel = {(g_out_w - pw) / 2, (g_out_h - ph) / 2, pw, ph};
    l.title_y = l.panel.y + (pad + title / 2) * u;
    float top = l.panel.y + (pad + title) * u;
    for (int c = 0; c < 2; ++c) {
        auto& items = c == 0 ? game : touch;
        float x = l.panel.x + (pad + (two ? c * (column + between) : 0)) * u;
        float y = two || c == 0 ? top : top + (section + (float)game.size() * (row + gap) + gap) * u;
        l.column_x[c] = x;
        l.section_y[c] = y + section / 2 * u;
        y += section * u;
        for (auto& [id, control] : items) {
            l.rows.push_back({id, control, c, {x, y, column * u, row * u}});
            y += (row + gap) * u;
        }
    }
    return l;
}

// The parts of a row's control: a choice's segments, or a slider's minus, track and plus.
static SDL_FRect control_area(const MenuLayout& l, const MenuRow& r) {
    float w = r.control == Control::Switch ? 52 * l.u : 0.58f * r.r.w;
    return {r.r.x + r.r.w - w - 10 * l.u, r.r.y + 9 * l.u, w, r.r.h - 18 * l.u};
}

static SDL_FRect segment(const MenuLayout& l, const MenuRow& r, int i) {
    SDL_FRect a = control_area(l, r);
    float w = a.w / choice_count(r.id);
    return {a.x + i * w, a.y, w, a.h};
}

static SDL_FRect slider_track(const MenuLayout& l, const MenuRow& r) {
    SDL_FRect a = control_area(l, r);
    return {a.x + a.h + 10 * l.u, a.y, a.w - 2 * (a.h + 10 * l.u), a.h};
}

static bool inside(const SDL_FRect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void menu_close() {
    g_menu_open = false;
    g_slider_finger = 0;
    for (auto& h : g_held_over) h = 0xFFFF;
}

static void menu_quit(bool to_saves) {
    if (to_saves) {
        if (FILE* f = std::fopen(g_cfg.after.c_str(), "w")) {
            std::fputs("saves\n", f);
            std::fclose(f);
        }
    }
    menu_close();
    g_quitting = true;
    state_stop(to_saves ? "Save files was chosen in the menu" : "Quit was chosen in the menu");
}

// Enter, A, or a tap on the row's label: an action acts, and the rest move on a step.
static void menu_activate(MenuId id) {
    switch (id) {
    case M_RESUME: menu_close(); break;
    case M_SAVES: menu_quit(true); break;
    case M_QUIT: menu_quit(false); break;
    case M_PAD: case M_STYLE: choose(id, choice_at(id) + 1); break;
    case M_SIZE: {
        int size = g_settings.number("touch", "size") + 10;
        pad_size_set(size > 200 ? 50 : size);
        break;
    }
    case M_MENU_BUTTON: setting_change("menu_button", g_settings.flag("touch", "menu_button") ? "false" : "true"); break;
    }
}

// Left and right: a step along a choice or the slider, a switch set off or on.
static void menu_adjust(MenuId id, int dir) {
    if (id == M_PAD || id == M_STYLE) choose(id, choice_at(id) + dir);
    else if (id == M_SIZE) pad_size_set(g_settings.number("touch", "size") + 10 * dir);
    else if (id == M_MENU_BUTTON) setting_change("menu_button", dir > 0 ? "true" : "false");
}

static void slide(const MenuLayout& l, const MenuRow& r, float x) {
    SDL_FRect t = slider_track(l, r);
    pad_size_set(50 + (int)std::lround(std::clamp((x - t.x) / t.w, 0.0f, 1.0f) * 150));
}

// A tap or click at (x, y), from `finger` (0 for the mouse).
static void menu_tap(float x, float y, SDL_FingerID finger) {
    MenuLayout l = menu_layout();
    for (size_t i = 0; i < l.rows.size(); ++i) {
        const MenuRow& r = l.rows[i];
        if (!inside(r.r, x, y)) continue;
        g_menu_at = (int)i;
        SDL_FRect a = control_area(l, r);
        if (r.control == Control::Choice && inside(a, x, y)) {
            for (int k = 0; k < choice_count(r.id); ++k)
                if (inside(segment(l, r, k), x, y)) choose(r.id, k);
        } else if (r.control == Control::Slider && inside(a, x, y)) {
            if (x < a.x + a.h + 5 * l.u) menu_adjust(r.id, -1);
            else if (x > a.x + a.w - a.h - 5 * l.u) menu_adjust(r.id, 1);
            else {
                slide(l, r, x);
                g_slider_finger = finger;
            }
        } else menu_activate(r.id);
        return;
    }
}

static void menu_open() {
    if (g_menu_open || g_quitting) return;
    g_menu_open = true;
    g_menu_at = 0;
    g_fingers.clear();
    g_lifted.clear();
    g_unread.clear();
}

static bool escape_is_a_button() {
    for (auto& [key, bit] : g_keys)
        if (key == SDL_SCANCODE_ESCAPE) return true;
    return false;
}

// ---- the events -------------------------------------------------------------------------------
// Up and down go through the rows, and left and right adjust one; on an action, they go to the other column.
static void on_menu_key(int dy, bool activate, bool back, int dx) {
    MenuLayout l = menu_layout();
    int n = (int)l.rows.size();
    g_menu_at = std::clamp(g_menu_at, 0, n - 1);
    const MenuRow& r = l.rows[g_menu_at];
    if (back) menu_close();
    else if (activate) menu_activate(r.id);
    else if (dx && r.control != Control::Action) menu_adjust(r.id, dx);
    else if (dx) {
        for (int i = 0; i < n; ++i)
            if (l.rows[i].column != r.column) { g_menu_at = i; break; }
    } else if (dy) g_menu_at = (g_menu_at + dy + n) % n;
}

static void on_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
        menu_close();
        state_stop("the window was closed");
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        pad_added(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_removed(e.gdevice.which);
        break;
    case SDL_EVENT_KEY_DOWN: {
        SDL_Scancode k = e.key.scancode;
        if (k == SDL_SCANCODE_F12 && !e.key.repeat) save_shot();
        if ((k == SDL_SCANCODE_F11 || (k == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))) && !e.key.repeat) {
            SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
            break;
        }
        bool back = k == SDL_SCANCODE_AC_BACK || (k == SDL_SCANCODE_ESCAPE && !escape_is_a_button());
        if (!g_menu_open) {
            if (back && !e.key.repeat) menu_open();
            break;
        }
        on_menu_key(k == SDL_SCANCODE_UP ? -1 : k == SDL_SCANCODE_DOWN ? 1 : 0,
                    !e.key.repeat && (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_SPACE), back && !e.key.repeat,
                    k == SDL_SCANCODE_LEFT ? -1 : k == SDL_SCANCODE_RIGHT ? 1 : 0);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        int b = e.gbutton.button;
        if (!g_menu_open) {
            if (b == SDL_GAMEPAD_BUTTON_BACK || b == SDL_GAMEPAD_BUTTON_GUIDE) menu_open();
            break;
        }
        on_menu_key(b == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0,
                    b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_START,
                    b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_BACK || b == SDL_GAMEPAD_BUTTON_GUIDE,
                    b == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? -1 : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1 : 0);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (g_menu_open && e.button.which != SDL_TOUCH_MOUSEID) {
            float x = e.button.x, y = e.button.y;
            SDL_RenderCoordinatesFromWindow(g_ren, e.button.x, e.button.y, &x, &y);
            menu_tap(x, y, 0);
        }
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION: {
        g_touchscreen = true;
        float x = e.tfinger.x * g_out_w, y = e.tfinger.y * g_out_h;
        if (g_menu_open) {
            if (e.type == SDL_EVENT_FINGER_DOWN) menu_tap(x, y, e.tfinger.fingerID);
            else if (g_slider_finger && e.tfinger.fingerID == g_slider_finger) {
                MenuLayout l = menu_layout();
                for (auto& r : l.rows)
                    if (r.id == M_SIZE) slide(l, r, x);
            }
            break;
        }
        TouchLayout l = pad_layout();
        if (e.type == SDL_EVENT_FINGER_DOWN && menu_button_shown() && touch_on_menu(l, x, y)) {
            menu_open();
            break;
        }
        if (!touch_shown()) break;
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            g_fingers[e.tfinger.fingerID] = {x, y, touch_on_dpad(l, x, y)};
            g_unread[e.tfinger.fingerID] = true;
        } else if (auto f = g_fingers.find(e.tfinger.fingerID); f != g_fingers.end()) {
            f->second.x = x;
            f->second.y = y;
        }
        break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (e.type == SDL_EVENT_FINGER_UP && g_unread.count(e.tfinger.fingerID)) g_lifted.push_back(e.tfinger.fingerID);
        else g_fingers.erase(e.tfinger.fingerID);
        if (e.tfinger.fingerID == g_slider_finger) g_slider_finger = 0;
        break;
    }
}

static void events() {
    if (!g_virtual.empty()) virtual_press();
    SDL_Event e;
    HostCall in("SDL_PollEvent");
    while (SDL_PollEvent(&e)) on_event(e);
    read_pads();
}

// ---- drawing ----------------------------------------------------------------------------------
static SDL_FColor rgba(float r, float g, float b, float a) { return {r, g, b, a}; }

static void fill_rounded(SDL_FRect r, float rad, SDL_FColor c) { ui_fill_rounded(r, rad, c); }
static void fill_circle(float x, float y, float r, SDL_FColor c) { ui_fill_circle(x, y, r, c); }
static void draw_text(float x, float y, float size, const std::string& s, SDL_FColor c) { ui_text(x, y, size, s, c, true); }

// The pads' colours: the black pad sold in the West, or the white Japanese one.
struct ButtonLook { SDL_FColor fill, label; };
static ButtonLook look_of(uint16_t bit, bool japanese) {
    const SDL_FColor light = rgba(0.92f, 0.92f, 0.94f, 1), dark = rgba(0.22f, 0.22f, 0.25f, 1);
    if (!japanese) {
        if (bit & (B_L | B_R)) return {rgba(0.36f, 0.36f, 0.39f, 1), light};
        if (bit == B_START) return {rgba(0.27f, 0.27f, 0.3f, 1), light};
        return {rgba(0.13f, 0.13f, 0.15f, 1), light};
    }
    switch (bit) {
    case B_A: return {rgba(0.2f, 0.66f, 0.45f, 1), light};
    case B_B: return {rgba(0.93f, 0.77f, 0.18f, 1), dark};
    case B_C: return {rgba(0.2f, 0.45f, 0.82f, 1), light};
    case B_X: case B_Y: case B_Z: return {rgba(0.8f, 0.8f, 0.83f, 1), dark};
    default: return {rgba(0.9f, 0.42f, 0.63f, 1), light};     // START, L and R
    }
}

static SDL_FColor faded(SDL_FColor c, bool held) {
    if (held) return {std::min(1.0f, c.r * 1.3f + 0.15f), std::min(1.0f, c.g * 1.3f + 0.15f), std::min(1.0f, c.b * 1.3f + 0.15f), 0.85f};
    return {c.r, c.g, c.b, 0.55f};
}

static void draw_touch_pad() {
    bool jp = g_settings.get("touch", "style") == "japanese";
    TouchLayout l = pad_layout();
    const SDL_FColor shade = rgba(0, 0, 0, 0.3f);

    // the d-pad: a cross, each arm lit while held
    float r = l.dpad_r, arm = 0.62f * r;
    SDL_FColor pad = jp ? rgba(0.42f, 0.42f, 0.45f, 1) : rgba(0.13f, 0.13f, 0.15f, 1);
    fill_circle(l.dpad_x, l.dpad_y, 1.08f * r, shade);
    fill_rounded({l.dpad_x - r, l.dpad_y - arm / 2, 2 * r, arm}, 0.15f * arm, faded(pad, false));
    fill_rounded({l.dpad_x - arm / 2, l.dpad_y - r, arm, 2 * r}, 0.15f * arm, faded(pad, false));
    const struct { uint16_t bit; float dx, dy; } arms[] = {{B_UP, 0, -1}, {B_DOWN, 0, 1}, {B_LEFT, -1, 0}, {B_RIGHT, 1, 0}};
    for (auto& a : arms) {
        float cx = l.dpad_x + a.dx * 0.62f * r, cy = l.dpad_y + a.dy * 0.62f * r;
        if (g_touch_held & a.bit) fill_rounded({cx - arm / 2, cy - arm / 2, arm, arm}, 0.15f * arm, faded(pad, true));
        fill_circle(cx + a.dx * 0.12f * r, cy + a.dy * 0.12f * r, 0.09f * r, rgba(1, 1, 1, 0.35f));
    }

    for (auto& b : l.buttons) {
        ButtonLook k = look_of(b.bit, jp);
        bool held = g_touch_held & b.bit;
        SDL_FColor label = {k.label.r, k.label.g, k.label.b, held ? 1.0f : 0.8f};
        if (b.shape == TouchButton::Round) {
            fill_circle(b.x, b.y, 1.1f * b.w, shade);
            fill_circle(b.x, b.y, b.w, faded(k.fill, held));
            draw_text(b.x, b.y, 0.8f * b.w, b.label, label);
        } else {
            float rad = b.shape == TouchButton::Pill ? b.h / 2 : 0.3f * b.h;
            fill_rounded({b.x - b.w / 2 - 4, b.y - b.h / 2 - 4, b.w + 8, b.h + 8}, rad + 4, shade);
            fill_rounded({b.x - b.w / 2, b.y - b.h / 2, b.w, b.h}, rad, faded(k.fill, held));
            draw_text(b.x, b.y, (b.shape == TouchButton::Pill ? 0.42f : 0.5f) * b.h, b.label, label);
        }
    }
}

static void draw_menu_button() {
    TouchLayout l = pad_layout();
    // the menu's button: three bars
    fill_circle(l.menu_x, l.menu_y, l.menu_r, rgba(0.13f, 0.13f, 0.15f, 0.55f));
    for (int i = -1; i <= 1; ++i)
        fill_rounded({l.menu_x - 0.5f * l.menu_r, l.menu_y + i * 0.32f * l.menu_r - 0.07f * l.menu_r, l.menu_r, 0.14f * l.menu_r},
                     0.07f * l.menu_r, rgba(1, 1, 1, 0.8f));
}

static void draw_menu() {
    const SDL_FColor panel = rgba(0.09f, 0.1f, 0.13f, 0.97f), row = rgba(0.15f, 0.16f, 0.2f, 1), well = rgba(0.1f, 0.11f, 0.14f, 1);
    const SDL_FColor accent = rgba(0.27f, 0.47f, 0.95f, 1), ring = rgba(0.6f, 0.74f, 1, 1), text = rgba(0.95f, 0.96f, 0.98f, 1);
    const SDL_FColor muted = rgba(0.6f, 0.63f, 0.69f, 1), knob = rgba(0.97f, 0.97f, 0.99f, 1), off = rgba(0.3f, 0.31f, 0.36f, 1);
    SDL_FRect all = {0, 0, (float)g_out_w, (float)g_out_h};
    SDL_SetRenderDrawColorFloat(g_ren, 0, 0, 0, 0.55f);
    SDL_RenderFillRect(g_ren, &all);
    MenuLayout l = menu_layout();
    float u = l.u;
    ui_fill_rounded(l.panel, 22 * u, panel);
    ui_text(l.panel.x + 20 * u, l.title_y, 26 * u, "Paused", text, true, UiAlign::Left);
    ui_text(l.panel.x + l.panel.w - 20 * u, l.title_y, 13 * u, "Back to resume", muted, false, UiAlign::Right);
    for (int c = 0; c < 2; ++c)
        ui_text(l.column_x[c], l.section_y[c], 12 * u, c == 0 ? "GAME" : "TOUCH CONTROLS", muted, true, UiAlign::Left);
    g_menu_at = std::clamp(g_menu_at, 0, (int)l.rows.size() - 1);
    for (size_t i = 0; i < l.rows.size(); ++i) {
        const MenuRow& r = l.rows[i];
        float cy = r.r.y + r.r.h / 2;
        if ((int)i == g_menu_at) ui_fill_rounded({r.r.x - 2 * u, r.r.y - 2 * u, r.r.w + 4 * u, r.r.h + 4 * u}, 14 * u, ring);
        ui_fill_rounded(r.r, 12 * u, r.id == M_RESUME ? accent : row);
        SDL_FColor label = r.id == M_QUIT ? rgba(1, 0.52f, 0.5f, 1) : text;
        ui_text(r.r.x + 16 * u, cy, 16 * u, menu_label(r.id), label, true, UiAlign::Left);
        SDL_FRect a = control_area(l, r);
        switch (r.control) {
        case Control::Action:
            if (r.id == M_SAVES) ui_text(r.r.x + r.r.w - 16 * u, cy, 24 * u, "›", muted, false, UiAlign::Right);
            break;
        case Control::Choice: {
            ui_fill_rounded(a, a.h / 2, well);
            int at = choice_at(r.id);
            for (int k = 0; k < choice_count(r.id); ++k) {
                SDL_FRect s = segment(l, r, k);
                if (k == at) ui_fill_rounded({s.x + 3 * u, s.y + 3 * u, s.w - 6 * u, s.h - 6 * u}, (s.h - 6 * u) / 2, accent);
                ui_text(s.x + s.w / 2, cy, 14 * u, choice_name(r.id, k), k == at ? text : muted, k == at);
            }
            break;
        }
        case Control::Slider: {
            int size = g_settings.number("touch", "size");
            float label_w = ui_text_width(16 * u, menu_label(r.id), true);
            ui_text(r.r.x + 16 * u + label_w + 10 * u, cy, 14 * u, std::to_string(size) + "%", muted, false, UiAlign::Left);
            float b = a.h / 2;
            ui_fill_circle(a.x + b, cy, b, well);
            ui_fill_circle(a.x + a.w - b, cy, b, well);
            ui_text(a.x + b, cy, 18 * u, "−", text, true);
            ui_text(a.x + a.w - b, cy, 18 * u, "+", text, true);
            SDL_FRect t = slider_track(l, r);
            float f = (size - 50) / 150.0f, x = t.x + f * t.w;
            ui_fill_rounded({t.x, cy - 3 * u, t.w, 6 * u}, 3 * u, off);
            ui_fill_rounded({t.x, cy - 3 * u, x - t.x, 6 * u}, 3 * u, accent);
            ui_fill_circle(x, cy, 10 * u, knob);
            break;
        }
        case Control::Switch: {
            bool on = g_settings.flag("touch", "menu_button");
            SDL_FRect p = {a.x, cy - 14 * u, a.w, 28 * u};
            ui_fill_rounded(p, 14 * u, on ? accent : off);
            ui_fill_circle(on ? p.x + p.w - 14 * u : p.x + 14 * u, cy, 11 * u, knob);
            break;
        }
        }
    }
}

static SDL_FRect g_dst;                         // where the picture goes

// While --resume plays its presses: a card saying what the run is getting to, and how far it has got,
// in place of the game going by at speed.
static void draw_catching_up() {
    static uint64_t from = sat_vblanks();
    float u = display_scale(), w = std::min(0.86f * g_out_w, 420 * u), h = 120 * u;
    SDL_FRect card = {(g_out_w - w) / 2, (g_out_h - h) / 2, w, h};
    ui_fill_rounded(card, 20 * u, rgba(0.09f, 0.1f, 0.13f, 1));
    std::string label = g_cfg.resume_label.empty() ? "Getting back to where you were" : g_cfg.resume_label;
    ui_text(card.x + w / 2, card.y + 40 * u, 18 * u, label, rgba(0.95f, 0.96f, 0.98f, 1), true);
    float done = g_cfg.resume > from ? (float)(sat_vblanks() - from) / (float)(g_cfg.resume - from) : 1;
    SDL_FRect track = {card.x + 28 * u, card.y + 76 * u, w - 56 * u, 8 * u};
    ui_fill_rounded(track, 4 * u, rgba(0.25f, 0.27f, 0.32f, 1));
    ui_fill_rounded({track.x, track.y, std::max(track.h, track.w * std::clamp(done, 0.0f, 1.0f)), track.h}, 4 * u, rgba(0.27f, 0.47f, 0.95f, 1));
}

// The last picture and what goes over it.
static void draw() {
    SDL_GetCurrentRenderOutputSize(g_ren, &g_out_w, &g_out_h);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    if (sat_resuming()) draw_catching_up();
    else if (g_tex) SDL_RenderTexture(g_ren, g_tex, nullptr, &g_dst);
    if (g_menu_open) draw_menu();
    else {
        if (touch_shown()) draw_touch_pad();
        if (menu_button_shown()) draw_menu_button();
    }
    if (g_quitting) {
        float u = display_scale();
        ui_fill_rounded({g_out_w / 2.0f - 70 * u, 24 * u, 140 * u, 40 * u}, 20 * u, rgba(0.09f, 0.1f, 0.13f, 0.9f));
        ui_text(g_out_w / 2.0f, 44 * u, 17 * u, "Saving…", rgba(1, 1, 1, 0.95f), true);
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
    HostCall in("SDL_RenderPresent");
    SDL_RenderPresent(g_ren);
}

// The menu, open until it is closed; the game waits.
static void menu_loop() {
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    ++g_host_paused;
    Clock::time_point t = Clock::now();
    while (g_menu_open) {
        SDL_Event e;
        {
            HostCall in("SDL_PollEvent");
            while (SDL_PollEvent(&e)) on_event(e);
        }
        draw();
        HostCall in("SDL_Delay");
        SDL_Delay(16);
    }
    g_base += Clock::now() - t;
    --g_host_paused;
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    read_pads();
}

static void present(const Frame& f) {
    g_last = &f;
    if (SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN)) {
        events();
        if (g_menu_open) menu_loop();
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
    // the TV's 4:3 screen, as large as the window allows, centred; a PAL TV
    // shows 256 lines, and a 224-line picture has a border above and below
    int sw = ww, sh = ww * 3 / 4;
    if (sh > wh) { sh = wh; sw = wh * 4 / 3; }
    int tv_lines = bios_pal() ? std::max(256, f.h) : f.h;
    int ph = sh * f.h / tv_lines;
    g_dst = {(float)((ww - sw) / 2), (float)((wh - ph) / 2), (float)sw, (float)ph};
    draw();
    events();
    if (g_menu_open) menu_loop();
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

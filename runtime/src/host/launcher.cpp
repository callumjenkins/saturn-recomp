// saturn-recomp runtime — the launcher: a menu in the window before the run starts, for the disc,
// the players, the controls, the display and the volume (Dear ImGui). A mouse, the keyboard or
// any gamepad drives it. What it sets is saved to the settings file at once (settings.h); Start
// hands the disc and the multitaps to the run, which opens in the same window. F12 saves the menu's
// picture (OUT/launcher.png); with SATURN_LAUNCHER_SHOT=FILE, its first frames go there and it closes,
// for a check without a display.
#include "host.h"
#include "host_sdl.h"
#include "video.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include <saturn/sh2.h>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace {

struct Disc { bool ok = false; std::string about; };

std::string field(const uint8_t* ip, int at, int len) {
    std::string s(reinterpret_cast<const char*>(ip + at), len);
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

// The disc is this build's when its 1st read file is the image a recompiled module was made from.
Disc check_disc(const std::string& cue) {
    if (cue.empty()) return {false, "No disc chosen"};
    if (!cdrom_open(cue)) return {false, "Cannot read " + cue};
    uint8_t raw[2352], ip[2048];
    if (!cdrom_read(150, raw)) return {false, "Cannot read the disc"};
    std::memcpy(ip, raw + 16, sizeof ip);
    if (std::memcmp(ip, "SEGA SEGASATURN ", 16)) return {false, "Not a Saturn disc"};
    std::string name = field(ip, 0x60, 0x70) + " (" + field(ip, 0x20, 10) + " " + field(ip, 0x2A, 6) + ")";
    uint32_t at = (uint32_t)ip[0xF0] << 24 | ip[0xF1] << 16 | ip[0xF2] << 8 | ip[0xF3], fad, size;
    std::string first = cdrom_first_file();
    if (first.empty() || !cdrom_find(first.c_str(), &fad, &size)) return {false, name + ": no 1st read file"};
    std::vector<uint8_t> image(size);
    for (uint32_t o = 0; o < size; o += 2048, ++fad) {
        if (!cdrom_read(fad, raw)) return {false, name + ": cannot read the 1st read file"};
        std::memcpy(image.data() + o, raw + 16, std::min<uint32_t>(2048, size - o));
    }
    for (int i = 0; i < g_sh2_nmodules; ++i) {
        const SH2Module* m = g_sh2_modules[i];
        if (m->base == at && m->size <= size && sh2_crc32(image.data(), m->size) == m->crc) return {true, name};
    }
    return {false, name + ": not the disc this game was built from"};
}

// SDL's file dialog answers on another thread.
std::mutex g_pick_lock;
std::string g_picked;

void SDLCALL picked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> hold(g_pick_lock);
    g_picked = files[0];
}

const struct { const char* key; const char* label; } kButtons[] = {
    {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"}, {"start", "Start"},
    {"a", "A"}, {"b", "B"}, {"c", "C"}, {"x", "X"}, {"y", "Y"}, {"z", "Z"}, {"l", "L"}, {"r", "R"}};

const char* const kTaps[] = {"No multitap: 2 players", "Multitap on port 1: 7 players", "Multitaps on both ports: 12 players"};

void save_picture(SDL_Renderer* ren, const std::string& path) {
    SDL_Surface* got = SDL_RenderReadPixels(ren, nullptr);
    SDL_Surface* px = got ? SDL_ConvertSurface(got, SDL_PIXELFORMAT_XRGB8888) : nullptr;
    if (px) {
        Frame f;
        f.w = px->w;
        f.h = px->h;
        f.px.resize((size_t)f.w * f.h);
        for (int y = 0; y < f.h; ++y)
            std::memcpy(&f.px[(size_t)y * f.w], (const uint8_t*)px->pixels + (size_t)y * px->pitch, (size_t)f.w * 4);
        write_png(path, f);
        sat_note("picture saved: %s", path.c_str());
    }
    SDL_DestroySurface(px);
    SDL_DestroySurface(got);
}

bool any_gamepad_input() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    bool held = false;
    for (int i = 0; i < n && !held; ++i) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !held; ++b) held = SDL_GetGamepadButton(g, (SDL_GamepadButton)b);
        held = held || SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > SDL_JOYSTICK_AXIS_MAX / 2 ||
               SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > SDL_JOYSTICK_AXIS_MAX / 2;
    }
    SDL_free(ids);
    return held;
}

class Launcher {
public:
    explicit Launcher(SaturnConfig& cfg) : cfg_(cfg), set_(host_settings()) {
        cue_ = cfg.cue.empty() ? set_.get("launcher", "cue") : cfg.cue;
        fixed_disc_ = !cfg.cue.empty();
        disc_ = check_disc(cue_);
        taps_ = cfg.multitap ? cfg.multitap : set_.number("launcher", "multitap");
    }

    // false: the player closed the window
    bool run() {
        SDL_Window* win = host_sdl_window();
        SDL_Renderer* ren = host_sdl_renderer();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        ImGui::StyleColorsDark();
        int ww = 0, wh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        float k = std::clamp(wh / 600.0f, 1.0f, 3.0f);
        ImGui::GetStyle().ScaleAllSizes(k);
        ImGui::GetStyle().FontScaleDpi = k;
        ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
        ImGui_ImplSDLRenderer3_Init(ren);
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
        SDL_SetWindowTitle(win, disc_.ok ? disc_.about.c_str() : "saturn-recomp");

        const char* shot = SDL_getenv("SATURN_LAUNCHER_SHOT");
        bool going = true;
        for (int frame = 0; going && !started_; ++frame) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_AC_BACK))
                    going = false;
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F12) want_picture_ = true;
                host_pad_event(e);
                if (!listen(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            }
            take_pick();
            // a gamepad's press that was just bound must not also press the menu
            bool quiet = !binding_.empty() || (settling_ && any_gamepad_input());
            if (!quiet) settling_ = false;
            if (quiet) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
            else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            draw();
            ImGui::Render();
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
            if (want_picture_) save_picture(ren, cfg_.out + "/launcher.png");
            want_picture_ = false;
            if (shot && frame == 3) {                // ImGui lays a window out over its first frames
                save_picture(ren, shot);
                going = false;
            }
            SDL_RenderPresent(ren);
            SDL_Delay(SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED) ? 50 : 8);
        }
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        if (!started_) return false;
        cfg_.cue = cue_;
        cfg_.multitap = taps_;
        return true;
    }

private:
    void save() {
        host_settings_save();
        host_rebind();
    }

    void take_pick() {
        std::string got;
        {
            std::lock_guard<std::mutex> hold(g_pick_lock);
            got.swap(g_picked);
        }
        if (got.empty()) return;
        cue_ = got;
        disc_ = check_disc(cue_);
        set_.set("launcher", "cue", cue_);
        save();
    }

    // While a button waits for its binding, the next key or gamepad button is that binding; true
    // when the event was taken.
    bool listen(const SDL_Event& e) {
        if (binding_.empty()) return false;
        std::string name;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) { binding_.clear(); return true; }
            if (binding_section_ == "keyboard") name = SDL_GetScancodeName(e.key.scancode);
        } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && binding_section_ == "gamepad") {
            name = SDL_GetGamepadStringForButton((SDL_GamepadButton)e.gbutton.button);
        } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && binding_section_ == "gamepad" &&
                   e.gaxis.value > SDL_JOYSTICK_AXIS_MAX / 2) {
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) name = "lefttrigger";
            if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) name = "righttrigger";
        } else {
            return e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_TEXT_INPUT;
        }
        if (name.empty()) return true;
        set_.set(binding_section_, binding_, name);
        binding_.clear();
        settling_ = true;
        save();
        return true;
    }

    void draw() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::BeginDisabled(!disc_.ok);
        if (ImGui::Button("  Start  ")) {
            set_.set("launcher", "multitap", std::to_string(taps_));
            save();
            started_ = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Quit")) {
            SDL_Event q{};
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(disc_.ok ? disc_.about.c_str() : "");

        if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            disc_and_players();
            display_and_sound();
            ImGui::TableNextColumn();
            controls();
            ImGui::EndTable();
        }
        ImGui::End();
    }

    void disc_and_players() {
        ImGui::SeparatorText("Disc");
        ImGui::TextWrapped("%s", cue_.empty() ? "(none)" : cue_.c_str());
        if (!disc_.ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", disc_.about.c_str());
        ImGui::BeginDisabled(fixed_disc_);
        if (ImGui::Button("Choose a disc...")) {
            static const SDL_DialogFileFilter filter[] = {{"Saturn disc (.cue)", "cue"}};
            SDL_ShowOpenFileDialog(picked, nullptr, host_sdl_window(), filter, 1, nullptr, false);
        }
        ImGui::EndDisabled();
        if (fixed_disc_) {
            ImGui::SameLine();
            ImGui::TextDisabled("given by --cue");
        }

        ImGui::SeparatorText("Players");
        for (int t = 0; t < 3; ++t)
            if (ImGui::RadioButton(kTaps[t], taps_ == t)) taps_ = t;
        auto row = [](int player, const char* what, bool held) {
            ImGui::TextColored(held ? ImVec4(0.5f, 1, 0.5f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Text), "Player %d: %s",
                               player, what);
        };
        const bool* keys = SDL_GetKeyboardState(nullptr);
        bool typing = false;
        for (int c = 0; c < SDL_SCANCODE_COUNT && !typing; ++c) typing = keys[c];
        row(1, "keyboard", typing && binding_.empty());
        for (auto& c : host_controllers()) row(c.slot + 1, c.name.c_str(), c.held);
        ImGui::TextDisabled("Press a button on a gamepad to see its player.");

    }

    void controls() {
        ImGui::SeparatorText("Controls");
        if (!binding_.empty())
            ImGui::TextColored(ImVec4(1, 0.9f, 0.4f, 1), "Press the %s for %s (Esc cancels)",
                               binding_section_ == "keyboard" ? "key" : "gamepad button", label(binding_));
        else
            ImGui::TextWrapped("Choose a binding, then press its new key or button. Every gamepad uses these.");
        if (ImGui::BeginTable("controls", 3, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Saturn");
            ImGui::TableSetupColumn("Keyboard");
            ImGui::TableSetupColumn("Gamepad");
            ImGui::TableHeadersRow();
            for (auto& b : kButtons) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(b.label);
                for (const char* section : {"keyboard", "gamepad"}) {
                    ImGui::TableNextColumn();
                    std::string id = set_.get(section, b.key) + "##" + section + b.key;
                    bool waiting = binding_ == b.key && binding_section_ == section;
                    if (ImGui::Button(waiting ? (std::string("...##") + section + b.key).c_str() : id.c_str(),
                                      ImVec2(-FLT_MIN, 0)) && binding_.empty()) {
                        binding_ = b.key;
                        binding_section_ = section;
                    }
                }
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Default controls")) {
            Settings defaults;
            for (const char* section : {"keyboard", "gamepad"})
                for (auto& key : defaults.keys(section)) set_.set(section, key, defaults.get(section, key));
            save();
        }
    }

    void display_and_sound() {
        ImGui::SeparatorText("Display and sound");
        bool full = set_.flag("display", "fullscreen");
        if (ImGui::Checkbox("Fullscreen (F11)", &full)) {
            set_.set("display", "fullscreen", full ? "true" : "false");
            SDL_SetWindowFullscreen(host_sdl_window(), full);
            save();
        }
        int scale = set_.number("display", "scale");
        if (ImGui::SliderInt("Window size", &scale, 1, 8, "%dx")) {
            set_.set("display", "scale", std::to_string(scale));
            if (!full) SDL_SetWindowSize(host_sdl_window(), 320 * scale, 240 * scale);
            save();
        }
        int volume = set_.number("audio", "volume");
        if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%")) {
            set_.set("audio", "volume", std::to_string(volume));
            save();
        }
    }

    static const char* label(const std::string& key) {
        for (auto& b : kButtons)
            if (key == b.key) return b.label;
        return key.c_str();
    }

    SaturnConfig& cfg_;
    Settings& set_;
    std::string cue_;
    bool fixed_disc_;
    Disc disc_;
    int taps_;
    bool started_ = false;
    bool want_picture_ = false;
    std::string binding_, binding_section_;     // the Saturn button waiting for a new key or gamepad button
    bool settling_ = false;                     // a gamepad binding was just taken: its button may still be held
};

}  // namespace

bool host_launch(SaturnConfig& cfg) {
    if (!host_window(cfg)) return false;
    return Launcher(cfg).run();
}

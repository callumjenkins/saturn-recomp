// saturn-recomp runtime — the player's settings (settings.h).
#include "settings.h"
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

static const char kDefaults[] = R"(; saturn-recomp's settings, read when a game opens its window. Delete a line to have its default
; back, or the whole file to have it written again. --fullscreen and --scale on the command line
; win over this file.

[display]
fullscreen = false
; the window is 320x240 times this
scale = 3

[audio]
; percent
volume = 100

; Player 1's keys, by SDL's key names (https://wiki.libsdl.org/SDL3/SDL_Scancode without
; "SDL_SCANCODE_"): several keys for one button go comma-separated.
[keyboard]
up = Up
down = Down
left = Left
right = Right
start = Return
a = Z
b = X
c = C
x = A
y = S
z = D
l = Q
r = W

; Every gamepad's buttons, each by its position on an Xbox-style pad: a b x y (bottom, right,
; left, top), leftshoulder, rightshoulder, lefttrigger, righttrigger, back, start, leftstick,
; rightstick, dpup, dpdown, dpleft, dpright. A section [gamepad GUID] holds one model's own,
; the GUID the log gives when it connects; its keys stand in for these.
[gamepad]
up = dpup
down = dpdown
left = dpleft
right = dpright
start = start
a = a
b = b
c = rightshoulder
x = x
y = y
z = leftshoulder
l = lefttrigger
r = righttrigger
; the left stick moves as the d-pad does once pushed this far, in percent
stick = 50
; a trigger presses its button once pulled this far, in percent
trigger = 50

; A Saturn pad on the screen for player 1, and the menu's button (the menu also changes these).
[touch]
; auto shows it on a touchscreen while no gamepad is connected; on, or off
controls = auto
; western: the black pad; japanese: the white pad, its A B C green, yellow and blue
style = western
; the pad's size, in percent
size = 100
; the menu's button at the top of a touchscreen, shown with the pad or without it
menu_button = true

; What the launcher (saturn --launcher) last started with.
[launcher]
; 6-player multitaps: 0 for none, 1 on port 1, 2 on both ports
multitap = 0
)";

// The values a word may take, by key.
static const struct { const char* key; const char* values[3]; } kChoices[] = {
    {"controls", {"auto", "on", "off"}}, {"style", {"western", "japanese"}}};

// The range a number may take, by key.
static const struct { const char* key; int lo, hi; } kRanges[] = {
    {"scale", 1, 8}, {"size", 50, 200}, {"volume", 0, 100}, {"stick", 5, 95}, {"trigger", 5, 95}, {"multitap", 0, 2}};

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Each "key = value" of the text, under its section.
template <typename F>
static void each_line(const std::string& text, F take) {
    std::istringstream in(text);
    std::string line, section;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = trim(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        take(n, section, eq == std::string::npos ? "" : trim(line.substr(0, eq)),
             eq == std::string::npos ? line : trim(line.substr(eq + 1)));
    }
}

Settings::Settings() {
    each_line(kDefaults, [&](int, const std::string& section, const std::string& key, const std::string& value) {
        values_[section][key] = value;
        order_[section].push_back(key);
    });
}

const char* Settings::default_text() { return kDefaults; }

std::string Settings::kind_of(const std::string& section) {
    return section.rfind("gamepad ", 0) == 0 ? "gamepad" : section;
}

void Settings::read(const std::string& text, std::vector<std::string>& problems) {
    each_line(text, [&](int n, const std::string& section, const std::string& key, const std::string& value) {
        std::string at = "settings line " + std::to_string(n) + ": ";
        auto known = values_.find(kind_of(section));
        if (known == values_.end()) { problems.push_back(at + "no section [" + section + "]"); return; }
        if (key.empty()) { problems.push_back(at + "not key = value"); return; }
        auto def = known->second.find(key);
        if (def == known->second.end()) { problems.push_back(at + "no setting " + key + " in [" + section + "]"); return; }
        if (def->second == "true" || def->second == "false") {
            if (value != "true" && value != "false") { problems.push_back(at + key + " is true or false"); return; }
        }
        for (auto& c : kChoices)
            if (key == c.key && std::none_of(std::begin(c.values), std::end(c.values),
                                             [&](const char* v) { return v && value == v; })) {
                problems.push_back(at + "no " + key + " " + value);
                return;
            }
        for (auto& r : kRanges)
            if (key == r.key) {
                char* end = nullptr;
                long v = std::strtol(value.c_str(), &end, 10);
                if (value.empty() || *end || v < r.lo || v > r.hi) {
                    problems.push_back(at + key + " is a number from " + std::to_string(r.lo) + " to " + std::to_string(r.hi));
                    return;
                }
            }
        if (!values_.count(section)) {                  // a model's own section starts as a copy of the defaults
            values_[section] = known->second;
            order_[section] = order_[kind_of(section)];
        }
        values_[section][key] = value;
    });
}

void Settings::set(const std::string& section, const std::string& key, const std::string& value) {
    if (!values_.count(section)) {
        values_[section] = values_[kind_of(section)];
        order_[section] = order_[kind_of(section)];
    }
    values_[section][key] = value;
}

std::string Settings::write_over(const std::string& text) const {
    static const Settings defaults;
    std::vector<std::string> lines;
    std::map<std::string, size_t> section_end;          // the line after each section's last
    std::map<std::string, std::map<std::string, bool>> written;
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (!t.empty() && t[0] == '[') section = trim(t.substr(1, t.find(']') - 1));
        size_t eq = t.find('=');
        if (!t.empty() && t[0] != ';' && t[0] != '#' && t[0] != '[' && eq != std::string::npos) {
            std::string key = trim(t.substr(0, eq));
            auto s = values_.find(section);
            if (s != values_.end() && s->second.count(key)) {
                const std::string& v = s->second.at(key);
                if (trim(t.substr(eq + 1)) != v) line = key + " =" + (v.empty() ? "" : " " + v);
                written[section][key] = true;
            }
        }
        lines.push_back(line);
        if (!t.empty() && t[0] != ';' && t[0] != '#') section_end[section] = lines.size();
    }
    // the values the text lacks, where they differ from the defaults: in their section, or a new one at the end
    std::map<size_t, std::vector<std::string>> insert;
    std::string tail;
    for (auto& [name, keys] : values_) {
        std::string added;
        for (auto& key : order_.at(name))
            if (!written[name][key] && keys.at(key) != defaults.get(name, key))
                added += key + " = " + keys.at(key) + "\n";
        if (added.empty()) continue;
        auto end = section_end.find(name);
        if (end == section_end.end()) tail += "\n[" + name + "]\n" + added;
        else insert[end->second].push_back(added);
    }
    std::string out;
    for (size_t i = 0; i <= lines.size(); ++i) {
        for (auto& a : insert[i]) out += a;
        if (i < lines.size()) out += lines[i] + "\n";
    }
    return out + tail;
}

std::string Settings::get(const std::string& section, const std::string& key) const {
    auto s = values_.find(section);
    if (s == values_.end()) s = values_.find(kind_of(section));
    if (s == values_.end()) return "";
    auto v = s->second.find(key);
    return v == s->second.end() ? "" : v->second;
}

bool Settings::flag(const std::string& section, const std::string& key) const { return get(section, key) == "true"; }
int Settings::number(const std::string& section, const std::string& key) const { return std::atoi(get(section, key).c_str()); }
bool Settings::has_section(const std::string& section) const { return values_.count(section) != 0; }

std::vector<std::string> Settings::keys(const std::string& section) const {
    auto o = order_.find(section);
    if (o == order_.end()) o = order_.find(kind_of(section));
    return o == order_.end() ? std::vector<std::string>{} : o->second;
}

std::string settings_load(const std::string& path, bool& found) {
    std::ifstream f(path);
    found = (bool)f;
    std::stringstream ss;
    if (f) ss << f.rdbuf();
    return ss.str();
}

bool settings_store(const std::string& path, const std::string& text) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << text) || !f.flush()) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

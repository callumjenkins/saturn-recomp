// saturn-recomp runtime — achievements (achievements.h).
#include "achievements.h"
#include "saturn.h"
#include "host.h"
#include "rc_error.h"
#include "rc_runtime.h"
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

const char* const kFormat = "saturn-recomp-achievements/1";

const SH2Achievement* g_defs;
int g_n;
std::string g_title;
rc_runtime_t g_rc;
bool g_rc_live;
std::string g_path;                         // achievements.json; empty: this run records nothing
std::string g_unrecorded;                   // why it records nothing, for the card
void (*g_show_card)(const char*, const char*) = host_notify;
std::map<std::string, std::string> g_unlocked;   // id: UTC time

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    char out[32];
    std::strftime(out, sizeof out, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return out;
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') out += '\\', out += (char)ch;
        else if (ch < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", ch);
            out += esc;
        } else out += (char)ch;
    }
    return out + "\"";
}

std::string file_text() {
    const DiscId& disc = bios_disc_id();
    std::string out = "{\n  \"format\": " + json_string(kFormat) + ",\n  \"game\": {\"product\": " + json_string(disc.product) +
                      ", \"version\": " + json_string(disc.version) + ", \"title\": " + json_string(g_title) + "},\n  \"achievements\": [";
    for (int i = 0; i < g_n; ++i)
        out += std::string(i ? "," : "") + "\n    {\"id\": " + json_string(g_defs[i].id) + ", \"title\": " + json_string(g_defs[i].title) +
               ", \"description\": " + json_string(g_defs[i].description) + ", \"points\": " + std::to_string(g_defs[i].points) + "}";
    out += "\n  ],\n  \"unlocked\": {";
    bool first = true;
    for (const auto& [id, when] : g_unlocked) {
        out += std::string(first ? "" : ",") + "\n    " + json_string(id) + ": " + json_string(when);
        first = false;
    }
    return out + (g_unlocked.empty() ? "}\n}\n" : "\n  }\n}\n");
}

// JSON, as far as reading achievements.json back needs: strings kept, other values skipped.
struct JsonReader {
    const char* p;
    const char* end;

    void space() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    bool take(char c) {
        space();
        if (p == end || *p != c) return false;
        ++p;
        return true;
    }
    bool string(std::string& out) {
        out.clear();
        if (!take('"')) return false;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { out += c; continue; }
            if (p == end) return false;
            switch (char e = *p++) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned u = 0;
                for (int k = 0; k < 4; ++k, ++p) {
                    if (p == end || !std::isxdigit((unsigned char)*p)) return false;
                    u = u << 4 | (unsigned)(std::isdigit((unsigned char)*p) ? *p - '0' : std::tolower((unsigned char)*p) - 'a' + 10);
                }
                if (u < 0x80) out += (char)u;
                else if (u < 0x800) out += (char)(0xC0 | u >> 6), out += (char)(0x80 | (u & 0x3F));
                else out += (char)(0xE0 | u >> 12), out += (char)(0x80 | (u >> 6 & 0x3F)), out += (char)(0x80 | (u & 0x3F));
                break;
            }
            default: out += e;              // " \ /
            }
        }
        return take('"');
    }
    bool skip() {
        space();
        if (p == end) return false;
        std::string s;
        if (*p == '"') return string(s);
        if (take('{')) {
            if (take('}')) return true;
            do if (!string(s) || !take(':') || !skip()) return false;
            while (take(','));
            return take('}');
        }
        if (take('[')) {
            if (take(']')) return true;
            do if (!skip()) return false;
            while (take(','));
            return take(']');
        }
        const char* from = p;                   // a number, true, false or null
        while (p < end && (std::isalnum((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')) ++p;
        return p > from;
    }
};

// The file's format and unlocks; false if it is not a JSON object, or its format is not a string, or its
// unlocks are not an object of strings.
bool parse_file(const std::string& text, std::string& format, std::map<std::string, std::string>& unlocked) {
    JsonReader j{text.data(), text.data() + text.size()};
    if (!j.take('{')) return false;
    std::string key, id, when;
    if (!j.take('}')) {
        do {
            if (!j.string(key) || !j.take(':')) return false;
            if (key == "format") {
                if (!j.string(format)) return false;
            } else if (key == "unlocked") {
                if (!j.take('{')) return false;
                if (j.take('}')) continue;
                do {
                    if (!j.string(id) || !j.take(':') || !j.string(when)) return false;
                    unlocked[id] = when;
                } while (j.take(','));
                if (!j.take('}')) return false;
            } else if (!j.skip()) return false;
        } while (j.take(','));
        if (!j.take('}')) return false;
    }
    j.space();
    return j.p == j.end;
}

// The file's unlocks into g_unlocked and its text into `text`; false if it is there but not one this
// runtime can add to.
bool read_file(std::string& text) {
    std::ifstream f(g_path, std::ios::binary);
    if (!f) return !fs::exists(g_path);
    std::stringstream in;
    in << f.rdbuf();
    text = in.str();
    std::string format;
    return parse_file(text, format, g_unlocked) && format == kFormat;
}

// Written beside the file and renamed over it, so a reader sees the last whole one.
void write_file() {
    std::error_code ec;
    fs::path path(g_path);
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::string tmp = g_path + ".new", text = file_text();
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { sat_note("achievements: cannot write %s", tmp.c_str()); return; }
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = std::fclose(f) == 0 && ok;
    if (ok) fs::rename(tmp, g_path, ec);
    if (!ok || ec) sat_note("achievements: cannot write %s", g_path.c_str());
}

uint32_t saturn_address(uint32_t rc) { return rc < 0x100000 ? 0x00200000 + rc : 0x06000000 + (rc - 0x100000); }

// The bytes in address order: rcheevos assembles a value from them as a little-endian console's, so a
// trigger reads a Saturn word through rcheevos's big-endian sizes. A byte outside work RAM reads 0.
uint32_t read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void*) {
    uint32_t got = 0;
    for (uint32_t i = 0; i < num_bytes; ++i) {
        buffer[i] = 0;
        uint32_t rc = address + i;
        got += rc < 0x200000 && sh2_mem_read(saturn_address(rc), &buffer[i], 1);
    }
    return got;
}

void on_event(const rc_runtime_event_t* e) {
    if (e->type != RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) return;
    const SH2Achievement& a = g_defs[e->id];
    sat_note("achievement %s triggered", a.id);
    if (!g_path.empty()) {
        g_unlocked[a.id] = utc_now();
        write_file();
    }
    std::string text = a.description;
    if (!g_unrecorded.empty()) text += (text.empty() ? "Not recorded (" : "\nNot recorded (") + g_unrecorded + ")";
    g_show_card(a.title, text.c_str());
}

// The option that keeps this run's unlocks out of the file, such as "--save -"; nullptr if none does.
const char* achievements_lockout(const SaturnConfig& cfg) {
    if (cfg.save == "-") return "--save -";
    if (cfg.hooks) return "--hook";
    if (!cfg.writes.empty()) return "--write";
    if (!cfg.state_in.empty()) return "--state-in";
    if (!cfg.input.empty()) return "--input";
    if (cfg.agent_fd >= 0) return "--agent";
    if (!cfg.cheats.empty()) return "--cheat";
    if (!cfg.virtual_input.empty()) return "--virtual-input";
    return nullptr;
}

}  // namespace

std::string achievements_start(const SH2Achievement* defs, int n, const char* title) {
    g_title = title;
    g_unlocked.clear();
    g_unrecorded.clear();
    g_path = bios_save_path().empty() ? "" : (fs::path(bios_save_path()).parent_path() / "achievements.json").string();
    if (g_rc_live) rc_runtime_destroy(&g_rc);
    rc_runtime_init(&g_rc);
    g_rc_live = true;
    g_defs = defs;
    g_n = n;
    if (!n) return "";
    std::string on_disk;
    if (const char* lockout = achievements_lockout(g_cfg)) {
        sat_note("achievements: not recorded (%s)", lockout);
        g_unrecorded = lockout;
        g_path.clear();
    } else if (g_path.empty()) {
        sat_note("achievements: not recorded (no user data directory)");
        g_unrecorded = "no user data directory";
    } else if (!read_file(on_disk)) {
        sat_note("achievements: not recorded (%s cannot be read as a %s file)", g_path.c_str(), kFormat);
        g_unrecorded = "achievements.json cannot be read";
        g_path.clear();
        g_unlocked.clear();
    }
    for (int i = 0; i < n; ++i) {
        int r = rc_runtime_activate_achievement(&g_rc, (uint32_t)i, defs[i].trigger, nullptr, 0);
        if (r != RC_OK) return std::string("achievement ") + defs[i].id + ": " + rc_error_str(r) + ": " + defs[i].trigger;
        if (g_unlocked.count(defs[i].id)) rc_runtime_deactivate_achievement(&g_rc, (uint32_t)i);
    }
    if (!g_path.empty() && file_text() != on_disk) write_file();      // so it lists the build's achievements before any unlocks
    return "";
}

void achievements_cards_for_tests(void (*show)(const char* title, const char* text)) {
    g_show_card = show ? show : host_notify;
}

void achievements_frame() {
    if (g_n) rc_runtime_do_frame(&g_rc, on_event, read_memory, nullptr, nullptr);
}

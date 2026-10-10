// saturn-recomp runtime — the player's settings (settings.ini in the user's data directory): the
// window, the volume and the controls, read by a run with a window (host.cpp).
//
// An INI file: [section] lines, then "key = value", ";" or "#" starting a comment. Every key has
// a default, written out with its explanation when the file is missing; a key the file leaves out
// keeps its default, and a value that does not read keeps it too, with a note saying so.
#pragma once
#include <map>
#include <string>
#include <vector>

class Settings {
public:
    Settings();                                          // the defaults

    // The file's settings over the defaults; `problems` gets a line for each one not taken.
    void read(const std::string& text, std::vector<std::string>& problems);

    // The defaults as a file, every key explained, for a player to edit.
    static const char* default_text();

    // A value, for the launcher; a model's section starts as a copy of the defaults, as in a file.
    void set(const std::string& section, const std::string& key, const std::string& value);

    // The text with these settings in it: each key's line rewritten, its comments and order kept,
    // and a key it lacks added where its value is not the default.
    std::string write_over(const std::string& text) const;

    std::string get(const std::string& section, const std::string& key) const;
    bool flag(const std::string& section, const std::string& key) const;   // true or false
    int number(const std::string& section, const std::string& key) const;
    std::vector<std::string> keys(const std::string& section) const;      // in the defaults' order
    bool has_section(const std::string& section) const;

    // Sections the defaults lack but whose keys are a known section's: "gamepad GUID" takes
    // "gamepad"'s keys, for one model's own bindings.
    static std::string kind_of(const std::string& section);

private:
    std::map<std::string, std::map<std::string, std::string>> values_;
    std::map<std::string, std::vector<std::string>> order_;
};

// The file's text, or "" when it cannot be read; written whole through a temporary file and a
// rename, so a crash leaves the old file or the new one, never half of one.
std::string settings_load(const std::string& path, bool& found);
// The text with the first default gamepad layout, untouched as the defaults wrote it, swapped for the
// current one; any other text as it is.
std::string settings_upgrade(const std::string& text);
bool settings_store(const std::string& path, const std::string& text);

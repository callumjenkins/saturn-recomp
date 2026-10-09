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

    void set(const std::string& section, const std::string& key, const std::string& value);
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
// The file's text with one setting changed in place, or added to its section, which is added at the
// end if the file lacks it; the rest of the text, its comments included, stays as it was.
std::string settings_with(const std::string& text, const std::string& section, const std::string& key,
                          const std::string& value);
bool settings_store(const std::string& path, const std::string& text);

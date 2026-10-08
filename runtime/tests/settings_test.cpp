// The player's settings: defaults, a file over them, values that do not read, and the write.
#include "settings.h"
#include "test.h"
#include <filesystem>
#include <string>
#include <vector>

TEST(settings_default_to_the_documented_file) {
    Settings s;
    CHECK_EQ(s.number("display", "scale"), 3);
    CHECK_EQ(s.flag("display", "fullscreen"), false);
    CHECK_EQ(s.get("keyboard", "start") == "Return", true);
    CHECK_EQ(s.get("gamepad", "l") == "lefttrigger", true);
    CHECK_EQ(s.keys("keyboard").size(), 13);
}

TEST(a_file_sets_what_it_names_and_leaves_the_rest) {
    Settings s;
    std::vector<std::string> problems;
    s.read("[display]\nscale = 2\n; a comment\n[keyboard]\na = Space, J\n", problems);
    CHECK_EQ(problems.size(), 0);
    CHECK_EQ(s.number("display", "scale"), 2);
    CHECK_EQ(s.get("keyboard", "a") == "Space, J", true);
    CHECK_EQ(s.get("keyboard", "b") == "X", true);
}

TEST(values_that_do_not_read_keep_their_default) {
    Settings s;
    std::vector<std::string> problems;
    s.read("[display]\nscale = 40\nfullscreen = yes\nsize = 2\n[sound]\nvolume = 5\nnonsense\n", problems);
    CHECK_EQ(problems.size(), 5);
    CHECK_EQ(s.number("display", "scale"), 3);
    CHECK_EQ(s.flag("display", "fullscreen"), false);
    CHECK_EQ(problems[0].find("line 2") != std::string::npos, true);
}

TEST(one_models_gamepad_section_starts_from_the_gamepad_defaults) {
    Settings s;
    std::vector<std::string> problems;
    s.read("[gamepad 0300abcd]\na = b\nstick = 30\n", problems);
    CHECK_EQ(problems.size(), 0);
    CHECK_EQ(s.has_section("gamepad 0300abcd"), true);
    CHECK_EQ(s.get("gamepad 0300abcd", "a") == "b", true);
    CHECK_EQ(s.get("gamepad 0300abcd", "c") == "rightshoulder", true);
    CHECK_EQ(s.number("gamepad 0300abcd", "stick"), 30);
    CHECK_EQ(s.get("gamepad", "a") == "a", true);
}

TEST(changed_settings_are_written_over_the_players_file) {
    Settings s;
    std::vector<std::string> problems;
    std::string file = "; mine\n[display]\nscale = 2\n\n[keyboard]\na = Space\n";
    s.read(file, problems);
    s.set("display", "scale", "4");
    s.set("display", "fullscreen", "true");
    s.set("launcher", "multitap", "2");
    s.set("gamepad 0300abcd", "a", "b");
    std::string out = s.write_over(file);
    CHECK_EQ(out == "; mine\n[display]\nscale = 4\nfullscreen = true\n\n[keyboard]\na = Space\n"
                    "\n[gamepad 0300abcd]\na = b\n\n[launcher]\nmultitap = 2\n", true);
    Settings back;
    back.read(out, problems);
    CHECK_EQ(problems.size(), 0);
    CHECK_EQ(back.number("display", "scale"), 4);
    CHECK_EQ(back.get("gamepad 0300abcd", "c") == "rightshoulder", true);
    CHECK_EQ(Settings().write_over(Settings::default_text()) == Settings::default_text(), true);
}

TEST(settings_are_written_whole_and_read_back) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "saturn-settings-test";
    fs::remove_all(dir);
    std::string path = (dir / "nested" / "settings.ini").string();
    CHECK_EQ(settings_store(path, Settings::default_text()), true);
    CHECK_EQ(fs::exists(path + ".new"), false);
    bool found = false;
    std::string text = settings_load(path, found);
    CHECK_EQ(found, true);
    Settings s;
    std::vector<std::string> problems;
    s.read(text, problems);
    CHECK_EQ(problems.size(), 0);
    settings_load((dir / "missing.ini").string(), found);
    CHECK_EQ(found, false);
    fs::remove_all(dir);
}

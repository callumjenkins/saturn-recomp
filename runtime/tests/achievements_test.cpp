// A build's achievements: triggers tested once a VBlank-IN over work RAM, and the unlocks kept in
// achievements.json beside the saves, unless something else plays or changes the run.
#include "achievements.h"
#include "saturn.h"
#include "state.h"
#include "test.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#define close _close
#define tzset _tzset
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

// rcheevos's 0x100100, which a trigger names as 0xH100100
const uint32_t kStage = 0x06000100;

// A new run on a disc with this product number, saves from --save `save`.
void boot(const std::string& save) {
    uint8_t ip[0x100] = {};
    std::memcpy(ip, "SEGA SEGASATURN ", 16);
    std::memcpy(ip + 0x20, "T-1234H   ", 10);
    std::memcpy(ip + 0x2A, "V1.001", 6);
    g_cfg = SaturnConfig{};
    g_cfg.save = save;
    bios_select_save(ip);
}

void poke(uint32_t addr, std::initializer_list<uint8_t> bytes) {
    sh2_mem_write(addr, std::data(bytes), bytes.size());
}

// What fn writes to stderr, where sat_note writes.
std::string logged(const std::function<void()>& fn) {
    std::string path = test_dir("achievements-log") + "/stderr.txt";
    std::FILE* f = std::fopen(path.c_str(), "w+b");
    std::fflush(stderr);
    int saved = dup(fileno(stderr));
    dup2(fileno(f), fileno(stderr));
    fn();
    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    std::rewind(f);
    std::string out;
    char buf[256];
    for (size_t k; (k = std::fread(buf, 1, sizeof buf, f));) out.append(buf, k);
    std::fclose(f);
    return out;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

int count(const std::string& text, const std::string& part) {
    int n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}

std::string read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

const SH2Achievement kWorld[] = {
    {"clear-world-1", "World 1", "Clear the first world", 10, "0xH100100=2"},
    {"clear-world-2", "World 2", "Clear the second world", 20, "0xH100100=3"},
};

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// An achievements.json with these unlocks and no achievements listed.
std::string file_with(const std::string& unlocked) {
    return R"({
  "format": "saturn-recomp-achievements/1",
  "game": {"product": "T-1234H", "version": "V1.001", "title": "Test"},
  "achievements": [],
  "unlocked": {)" + unlocked + "}\n}\n";
}

// One test of the triggers with the stage byte at `stage`; what it logged.
std::string frame_at(uint8_t stage) {
    poke(kStage, {stage});
    return logged(achievements_frame);
}

// An environment variable set to `value` until the end of the scope; nullptr unsets it.
struct EnvFor {
    std::string name, before;
    bool had;
    EnvFor(const char* n, const char* value) : name(n) {
        const char* v = std::getenv(n);
        had = v;
        before = v ? v : "";
        set(value);
    }
    EnvFor(const EnvFor&) = delete;
    EnvFor& operator=(const EnvFor&) = delete;
    ~EnvFor() { set(had ? before.c_str() : nullptr); }
    void set(const char* v) {
#if defined(_WIN32)
        _putenv_s(name.c_str(), v ? v : "");
#else
        if (v) setenv(name.c_str(), v, 1);
        else unsetenv(name.c_str());
#endif
    }
};

// The cards shown while one is in scope, each its title and text.
struct Cards {
    static inline std::vector<std::pair<std::string, std::string>> shown;
    Cards() {
        shown.clear();
        achievements_cards_for_tests([](const char* title, const char* text) { shown.emplace_back(title, text); });
    }
    Cards(const Cards&) = delete;
    Cards& operator=(const Cards&) = delete;
    ~Cards() { achievements_cards_for_tests(nullptr); }
};

std::string utc(std::time_t t) {
    char out[32];
    std::strftime(out, sizeof out, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return out;
}

}  // namespace

TEST(a_trigger_that_fails_to_parse_stops_the_run_with_its_id) {
    boot("-");
    const SH2Achievement defs[] = {{"fine", "Fine", "", 5, "0xH100100=1"}, {"bad-one", "Bad", "", 5, "0xH100100?1"}};
    std::string error = achievements_start(defs, 2, "Test");
    CHECK_EQ(error == "achievement bad-one: Invalid operator: 0xH100100?1", true);
}

TEST(the_read_callback_returns_saturn_memory_as_rcheevos_reads_it) {
    boot("-");
    const SH2Achievement defs[] = {
        {"u8", "", "", 0, "0xH100100=h12"},
        {"u16-be", "", "", 0, "0xI100100=h1234"},
        {"u32-be", "", "", 0, "0xG100104=h12345678"},
        {"u16-le", "", "", 0, "0x 100100=h3412"},
        {"low-u16-be", "", "", 0, "0xI000010=hBEEF"},
    };
    CHECK_EQ(achievements_start(defs, 5, "Test").empty(), true);
    poke(kStage, {0, 0, 0, 0, 0, 0, 0, 0});
    poke(0x00200010, {0, 0});
    std::string before = logged(achievements_frame);           // each is false once, so each can trigger
    poke(kStage, {0x12, 0x34, 0, 0, 0x12, 0x34, 0x56, 0x78});
    poke(0x00200010, {0xBE, 0xEF});
    std::string after = logged(achievements_frame);
    CHECK_EQ(has(before, "triggered"), false);
    for (const char* id : {"u8", "u16-be", "u32-be", "u16-le", "low-u16-be"})
        CHECK_EQ(has(after, std::string("achievement ") + id + " triggered"), true);
}

TEST(an_achievement_triggers_once_and_goes_into_the_file_with_a_utc_time) {
    fs::path dir = test_dir("achievements-unlock");
    boot((dir / "backup.bin").string());
    CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
    std::string log = frame_at(0) + frame_at(2) + frame_at(2) + frame_at(0) + frame_at(2);
    CHECK_EQ(count(log, "achievement clear-world-1 triggered"), 1);
    CHECK_EQ(has(log, "clear-world-2"), false);
    std::string file = read(dir / "achievements.json");
    CHECK_EQ(std::regex_search(file, std::regex(R"re("unlocked": \{\s*"clear-world-1": "\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ"\s*\})re")), true);
}

TEST(an_achievement_already_in_the_file_never_triggers_again_or_changes_its_time) {
    fs::path dir = test_dir("achievements-again");
    write(dir / "achievements.json", file_with(R"("clear-world-1": "2020-01-02T03:04:05Z")"));
    boot((dir / "backup.bin").string());
    CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
    std::string log = frame_at(0) + frame_at(2) + frame_at(3);
    CHECK_EQ(has(log, "clear-world-1"), false);
    CHECK_EQ(has(log, "achievement clear-world-2 triggered"), true);
    std::string file = read(dir / "achievements.json");
    CHECK_EQ(has(file, R"("clear-world-1": "2020-01-02T03:04:05Z")"), true);
    CHECK_EQ(has(file, R"("clear-world-2": ")"), true);
}

TEST(with_save_dash_triggers_log_but_no_file_is_written) {
    fs::path data = use_data_dir(test_dir("achievements-data"));
    boot("-");
    std::string log = logged([] { CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true); });
    log += frame_at(0) + frame_at(2);
    CHECK_EQ(has(log, "achievements: not recorded (--save -)"), true);
    CHECK_EQ(has(log, "achievement clear-world-1 triggered"), true);
    CHECK_EQ(fs::exists(data / "saturn-recomp"), false);

    fs::path dir = test_dir("achievements-save-file");         // the same achievements, saves in a file
    boot((dir / "backup.bin").string());
    CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
    frame_at(0);
    frame_at(2);
    CHECK_EQ(has(read(dir / "achievements.json"), R"("clear-world-1": ")"), true);
}

TEST(each_of_hook_write_state_in_input_agent_cheat_and_virtual_input_stops_recording) {
    struct Lockout { const char* flag; void (*set)(SaturnConfig&); };
    const Lockout rows[] = {
        {"--hook", [](SaturnConfig& c) { c.hooks = true; }},
        {"--write", [](SaturnConfig& c) { c.writes = "10:06000100=02"; }},
        {"--state-in", [](SaturnConfig& c) { c.state_in = "dump.bin"; }},
        {"--input", [](SaturnConfig& c) { c.input = "10:START"; }},
        {"--agent", [](SaturnConfig& c) { c.agent_fd = 3; }},
        {"--cheat", [](SaturnConfig& c) { c.cheats = "MAIN:06000100=02"; }},
        {"--virtual-input", [](SaturnConfig& c) { c.virtual_input = "10:START"; }},
    };
    for (const Lockout& l : rows) {
        fs::path dir = test_dir("achievements-lockout");
        boot((dir / "backup.bin").string());
        l.set(g_cfg);
        std::string log = logged([] { CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true); });
        log += frame_at(0) + frame_at(2);
        CHECK_EQ(has(log, std::string("achievements: not recorded (") + l.flag + ")"), true);
        CHECK_EQ(has(log, "achievement clear-world-1 triggered"), true);
        CHECK_EQ(fs::exists(dir / "achievements.json"), false);
    }
}

TEST(a_run_with_no_lockout_flag_records_beside_the_default_saves) {
    fs::path data = use_data_dir(test_dir("achievements-default"));
    boot("");
    std::string log = logged([] { CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true); });
    frame_at(0);
    frame_at(2);
    CHECK_EQ(has(log, "not recorded"), false);
    fs::path saves = data / "saturn-recomp" / "T-1234H_V1.001";
    CHECK_EQ(bios_save_path() == (saves / "backup.bin").string(), true);
    CHECK_EQ(has(read(saves / "achievements.json"), R"("clear-world-1": ")"), true);
}

TEST(the_file_lists_every_definition_unlocked_or_not) {
    fs::path dir = test_dir("achievements-list");
    write(dir / "achievements.json", file_with(R"("clear-world-1": "2020-01-02T03:04:05Z")"));
    boot((dir / "backup.bin").string());
    CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
    CHECK_EQ(has(read(dir / "achievements.json"), R"("achievements": [
    {"id": "clear-world-1", "title": "World 1", "description": "Clear the first world", "points": 10},
    {"id": "clear-world-2", "title": "World 2", "description": "Clear the second world", "points": 20}
  ],)"), true);
}

TEST(an_unlocked_id_the_build_no_longer_defines_stays_in_the_file) {
    fs::path dir = test_dir("achievements-dropped");
    write(dir / "achievements.json", file_with(R"("from-an-older-build": "2020-01-02T03:04:05Z")"));
    boot((dir / "backup.bin").string());
    CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
    frame_at(0);
    frame_at(2);
    std::string file = read(dir / "achievements.json");
    CHECK_EQ(has(file, R"("from-an-older-build": "2020-01-02T03:04:05Z")"), true);
    CHECK_EQ(has(file, R"("clear-world-1": ")"), true);
    CHECK_EQ(has(file, R"({"id": "from-an-older-build")"), false);
}

TEST(the_file_is_replaced_by_rename_never_written_in_place) {
    fs::path dir = test_dir("achievements-rename");
    std::string before = file_with(R"("from-an-older-build": "2020-01-02T03:04:05Z")");
    write(dir / "achievements.json", before);
    boot((dir / "backup.bin").string());
    fs::create_directories(dir / "achievements.json.new");          // nowhere to write it beside itself
    std::string log = logged([] { CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true); });
    CHECK_EQ(has(log, "achievements: cannot write"), true);
    CHECK_EQ(read(dir / "achievements.json") == before, true);
    fs::remove(dir / "achievements.json.new");
#if !defined(_WIN32)                        // Windows refuses to rename over a file a reader has open
    std::ifstream reader(dir / "achievements.json", std::ios::binary);
    frame_at(0);
    frame_at(2);
    std::stringstream seen;
    seen << reader.rdbuf();
    CHECK_EQ(seen.str() == before, true);
    CHECK_EQ(has(read(dir / "achievements.json"), R"("clear-world-1": ")"), true);
#endif
    CHECK_EQ(fs::exists(dir / "achievements.json.new"), false);
}

TEST(a_build_with_no_achievements_runs_unchanged) {
    fs::path data = use_data_dir(test_dir("achievements-none"));
    boot("");
    std::string log = logged([] { CHECK_EQ(achievements_start(g_achievements, g_nachievements, g_game_title).empty(), true); });
    log += frame_at(0) + frame_at(2);
    CHECK_EQ(has(log, "achievement"), false);
    CHECK_EQ(fs::exists(data / "saturn-recomp" / "T-1234H_V1.001" / "achievements.json"), false);
}

TEST(a_trigger_that_fails_to_parse_stops_the_run_even_once_unlocked) {
    fs::path dir = test_dir("achievements-bad-unlocked");
    write(dir / "achievements.json", file_with(R"("bad-one": "2020-01-02T03:04:05Z")"));
    boot((dir / "backup.bin").string());
    const SH2Achievement defs[] = {{"bad-one", "Bad", "", 5, "0xH100100?1"}};
    CHECK_EQ(achievements_start(defs, 1, "Test") == "achievement bad-one: Invalid operator: 0xH100100?1", true);
}

TEST(a_file_the_runtime_wrote_reads_back_in_the_next_run) {
    static const SH2Achievement kOdd[] = {
        {"odd", "Say \"hi\" \\ back", "line one\nline two\ttab", 10, "0xH100100=2"},
    };
    fs::path dir = test_dir("achievements-round-trip");
    boot((dir / "backup.bin").string());
    CHECK_EQ(achievements_start(kOdd, 1, "A \"quoted\" title").empty(), true);
    std::string first = read(dir / "achievements.json");
    CHECK_EQ(has(first, R"("title": "Say \"hi\" \\ back")"), true);
    CHECK_EQ(has(first, R"("description": "line one\u000aline two\u0009tab")"), true);
    CHECK_EQ(has(first, R"("unlocked": {})"), true);
    for (int run = 0; run < 2; ++run) {         // read back with nothing unlocked, then with "odd"
        boot((dir / "backup.bin").string());
        std::string log = logged([] { CHECK_EQ(achievements_start(kOdd, 1, "A \"quoted\" title").empty(), true); });
        log += frame_at(0) + frame_at(2);
        CHECK_EQ(has(log, "not recorded"), false);
        CHECK_EQ(has(log, "achievement odd triggered"), run == 0);
    }
    CHECK_EQ(has(read(dir / "achievements.json"), R"("odd": ")"), true);
}

TEST(a_file_that_is_not_saturn_recomp_achievements_1_is_left_alone_and_its_unlocks_ignored) {
    const std::string ours = file_with(R"("clear-world-1": "2020-01-02T03:04:05Z")");
    struct NotOurs { const char* what; std::string text; };
    const NotOurs rows[] = {
        {"text after the object", ours + "x"},
        {"another format", std::regex_replace(ours, std::regex("achievements/1"), "achievements/2")},
    };
    for (const NotOurs& n : rows) {
        fs::path dir = test_dir("achievements-not-ours");
        write(dir / "achievements.json", n.text);
        boot((dir / "backup.bin").string());
        std::string log = logged([] { CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true); });
        log += frame_at(0) + frame_at(2);
        CHECK_EQ(has(log, "cannot be read as a saturn-recomp-achievements/1 file)"), true);
        CHECK_EQ(has(log, "achievement clear-world-1 triggered"), true);
        CHECK_EQ(read(dir / "achievements.json") == n.text, true);
    }
}

#if !defined(_WIN32)
TEST(a_file_it_may_not_read_is_left_alone) {
    if (geteuid() == 0) return;                 // root reads it anyway
    fs::path dir = test_dir("achievements-unreadable");
    std::string before = file_with(R"("clear-world-1": "2020-01-02T03:04:05Z")");
    write(dir / "achievements.json", before);
    fs::permissions(dir / "achievements.json", fs::perms::none);
    boot((dir / "backup.bin").string());
    std::string log = logged([] { CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true); });
    log += frame_at(0) + frame_at(2);
    fs::permissions(dir / "achievements.json", fs::perms::owner_read | fs::perms::owner_write);
    CHECK_EQ(has(log, "cannot be read as a saturn-recomp-achievements/1 file)"), true);
    CHECK_EQ(read(dir / "achievements.json") == before, true);
}
#endif

TEST(the_ends_of_each_work_ram_read_as_rcheevos_numbers_them) {
    boot("-");
    const SH2Achievement defs[] = {
        {"low-last", "", "", 0, "0xH0fffff=h34"},
        {"high-first", "", "", 0, "0xH100000=h12"},
        {"high-last", "", "", 0, "0xH1fffff=h56"},
        {"past-the-end", "", "", 0, "0xH200000=h12"},
    };
    CHECK_EQ(achievements_start(defs, 4, "Test").empty(), true);
    poke(0x002FFFFF, {0});
    poke(0x06000000, {0});
    poke(0x060FFFFF, {0});
    std::string before = logged(achievements_frame);
    poke(0x002FFFFF, {0x34});
    poke(0x06000000, {0x12});
    poke(0x060FFFFF, {0x56});
    std::string after = logged(achievements_frame);
    CHECK_EQ(has(before, "triggered"), false);
    for (const char* id : {"low-last", "high-first", "high-last"})
        CHECK_EQ(has(after, std::string("achievement ") + id + " triggered"), true);
    CHECK_EQ(has(after, "past-the-end"), false);
}

TEST(the_unlock_time_is_utc_whatever_the_time_zone) {
    fs::path dir = test_dir("achievements-utc");
    std::string from, to;
    {
        EnvFor tz("TZ", "UTC-14");
        tzset();
        boot((dir / "backup.bin").string());
        CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
        from = utc(std::time(nullptr));
        frame_at(0);
        frame_at(2);
        to = utc(std::time(nullptr));
    }
    tzset();
    std::smatch when;
    std::string file = read(dir / "achievements.json");
    CHECK_EQ(std::regex_search(file, when, std::regex(R"re("clear-world-1": "([^"]*)")re")), true);
    CHECK_EQ(from <= when[1].str() && when[1].str() <= to, true);
}

TEST(with_no_user_data_directory_triggers_log_but_nothing_is_recorded) {
    EnvFor appdata("APPDATA", nullptr), xdg("XDG_DATA_HOME", nullptr), home("HOME", nullptr);
    boot("");
    std::string log = logged([] { CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true); });
    log += frame_at(0) + frame_at(2);
    CHECK_EQ(has(log, "achievements: not recorded (no user data directory)"), true);
    CHECK_EQ(has(log, "achievement clear-world-1 triggered"), true);
}

TEST(each_vblank_in_tests_the_triggers) {
    State saved;
    video_state(saved);                         // the raster's position, put back for the tests after
    uint64_t line;                              // video_state writes the lines since power-on first
    std::memcpy(&line, saved.data.data(), sizeof line);
    const uint64_t frame = video_frame_ns(), now = line * (frame / 263);
    auto run_to = [](uint64_t t) {              // each tick stops at VBlank-OUT
        for (int i = 0; i < 10; ++i) video_tick(t);
    };
    boot("-");
    CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
    std::string log = logged([&] {
        poke(kStage, {0});
        run_to(now + frame);
        poke(kStage, {2});
        run_to(now + 2 * frame);
    });
    CHECK_EQ(has(log, "achievement clear-world-1 triggered"), true);
    saved.loading = true;
    video_state(saved);
}

TEST(a_card_from_a_run_that_records_has_the_description_alone) {
    fs::path dir = test_dir("achievements-card");
    boot((dir / "backup.bin").string());
    CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
    Cards cards;
    frame_at(0);
    frame_at(2);
    CHECK_EQ(cards.shown.size(), 1u);
    CHECK_EQ(cards.shown[0].first == "World 1", true);
    CHECK_EQ(cards.shown[0].second == "Clear the first world", true);
}

TEST(a_card_from_a_run_that_records_nothing_says_the_unlock_was_not_recorded_and_why) {
    struct Row { const char* why; void (*start)(); };
    const Row rows[] = {
        {"--save -", [] { boot("-"); }},
        {"--cheat", [] {
            boot((fs::path(test_dir("achievements-card-cheat")) / "backup.bin").string());
            g_cfg.cheats = "MAIN:06000100=02";
        }},
        {"no user data directory", [] {
            EnvFor appdata("APPDATA", nullptr), xdg("XDG_DATA_HOME", nullptr), home("HOME", nullptr);
            boot("");
        }},
        {"achievements.json cannot be read", [] {
            fs::path dir = test_dir("achievements-card-unread");
            write(dir / "achievements.json", "not json");
            boot((dir / "backup.bin").string());
        }},
    };
    const SH2Achievement plain[] = {{"plain", "Plain", "", 5, "0xH100100=2"}};
    for (const Row& r : rows) {
        r.start();
        CHECK_EQ(achievements_start(kWorld, 2, "Test").empty(), true);
        Cards cards;
        frame_at(0);
        frame_at(2);
        CHECK_EQ(cards.shown.size(), 1u);
        CHECK_EQ(cards.shown[0].first == "World 1", true);
        CHECK_EQ(cards.shown[0].second == std::string("Clear the first world\nNot recorded (") + r.why + ")", true);
    }
    boot("-");                                  // with no description, that line alone
    CHECK_EQ(achievements_start(plain, 1, "Test").empty(), true);
    Cards cards;
    frame_at(0);
    frame_at(2);
    CHECK_EQ(cards.shown.size(), 1u);
    CHECK_EQ(cards.shown[0].second == "Not recorded (--save -)", true);
}

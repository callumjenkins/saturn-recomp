// saturn-recomp runtime — the machine dumped to a file and loaded back.
//
// Recompiled code keeps the SH-2's calls on the host's stack, so a dump is taken only where the
// host's stack can be thrown away: a safe moment (state_point), where everything the run needs to go
// on is in the Saturn's memory and the devices, and the run goes on from one address. A game's own
// task switch is one (tasks.cpp): every parked task is in its jmp_buf, and the switch goes to one
// PC. A loaded dump starts there on a fresh task, as a task resumed somewhere other than where it
// parked always does.
//
// The file: "SATSTATE", the format's version, then the parts (state.h), each its name and its size, in
// the order they load (the VBlank count before the pads, which go on from it),
// so a part this build reads differently is refused rather than read wrong. A dump is written from a
// copy on a thread of its own, beside the file and renamed over it.
#include "saturn.h"
#include "state.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <thread>

static const char kMagic[8] = {'S', 'A', 'T', 'S', 'T', 'A', 'T', 'E'};
static const uint32_t kVersion = 1;

static const struct { const char* name; void (*fn)(State&); } kParts[] = {
    {"machine", machine_state}, {"video", video_state},     {"core", core_state},       {"mmio", mmio_state},
    {"bios", bios_state},       {"scu", scu_state},         {"scudsp", scudsp_state},   {"onchip", onchip_state},
    {"smpc", smpc_state},       {"cdblock", cdblock_state}, {"vdp1", vdp1_state},       {"vdp2", vdp2_state},
    {"sound", sound_state},     {"scsp", scsp_state},       {"tasks", tasks_state},
};

static std::string g_loading_path;

void state_fail(const char* what) {
    sat_fatal("state %s: %s", g_loading_path.empty() ? "dump" : g_loading_path.c_str(), what);
}

void core_state(State& s) {
    s.raw(g_wram_l, 0x100000);
    s.raw(g_wram_h, 0x100000);
    for (int i = 0; i < sh2_active_slots(); ++i) {
        const SH2Module* m = sh2_active_slot(i);
        std::string name = m ? m->name : "";
        s.str(name);
        if (!s.loading) continue;
        m = nullptr;
        for (int k = 0; k < g_sh2_nmodules && !name.empty(); ++k)
            if (name == g_sh2_modules[k]->name) m = g_sh2_modules[k];
        if (!name.empty() && !m) state_fail(("no module " + name + " in this build").c_str());
        sh2_set_active_slot(i, m);
    }
}

// ---- writing --------------------------------------------------------------------------------
static std::atomic<bool> g_writing;
static std::thread g_writer;
static bool g_wanted;
static uint64_t g_at_vblank = ~0ull;            // --state-at: the first safe moment from this VBlank on
static std::chrono::steady_clock::time_point g_next;
static bool g_points;                           // the run has safe moments (state_enable)
static const char* g_stop_why;                  // the run stops once this dump is written
static uint64_t g_stop_by;                      // ... or at this VBlank without one

static void put(std::vector<uint8_t>& out, const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

static void write_file(std::vector<uint8_t> bytes, std::string path, uint64_t vblank) {
    std::string part = path + ".part";
    FILE* f = std::fopen(part.c_str(), "wb");
    bool ok = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f) ok = std::fclose(f) == 0 && ok;
    std::error_code e;
    if (ok) std::filesystem::rename(part, path, e);
    if (!ok || e) sat_note("state: cannot write %s", path.c_str());
    else sat_note("state: VBlank %llu to %s, %zu KB", (unsigned long long)vblank, path.c_str(), bytes.size() >> 10);
    g_writing = false;
}

void state_enable() { g_points = true; }

void state_init() {
    if (!g_cfg.state_at.empty()) g_at_vblank = std::stoull(g_cfg.state_at);
    if (g_cfg.state_every) g_next = std::chrono::steady_clock::now() + std::chrono::seconds(g_cfg.state_every);
}

// From the master's polls: a dump is due by the host's clock, or a stop has waited long enough.
void state_poll() {
    if (g_cfg.state_out.empty()) return;
    if (sat_vblanks() >= g_at_vblank) g_wanted = true;
    if (g_cfg.state_every && std::chrono::steady_clock::now() >= g_next) {
        g_next = std::chrono::steady_clock::now() + std::chrono::seconds(g_cfg.state_every);
        g_wanted = true;
    }
    if (g_stop_why && sat_vblanks() >= g_stop_by) {
        sat_note("state: no safe moment came, so none was written at the stop");
        sat_stop(g_stop_why);
    }
}

void state_stop(const char* why) {
    if (g_cfg.state_out.empty() || !g_points) sat_stop(why);
    if (g_stop_why) return;
    g_stop_why = why;
    g_stop_by = sat_vblanks() + 120;
    g_wanted = true;
}

void state_point(SH2Context& c) {
    if (!g_wanted || g_writing || c.cpu != 0 || sat_in_interrupt() || state_slave_busy()) return;
    g_wanted = false;
    g_at_vblank = ~0ull;
    std::vector<uint8_t> out;
    put(out, kMagic, sizeof kMagic);
    put(out, &kVersion, sizeof kVersion);
    for (auto& p : kParts) {
        State s;
        p.fn(s);
        uint8_t n = (uint8_t)std::strlen(p.name);
        uint64_t size = s.data.size();
        put(out, &n, 1);
        put(out, p.name, n);
        put(out, &size, sizeof size);
        put(out, s.data.data(), s.data.size());
    }
    if (g_writer.joinable()) g_writer.join();
    g_writing = true;
    g_writer = std::thread(write_file, std::move(out), g_cfg.state_out, sat_vblanks());
    if (g_stop_why) {
        g_writer.join();
        sat_stop(g_stop_why);
    }
}

void state_finish() {
    if (g_writer.joinable()) g_writer.join();
}

// ---- loading --------------------------------------------------------------------------------
void state_load(const std::string& path) {
    g_loading_path = path;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) state_fail("cannot open it");
    std::vector<uint8_t> bytes;
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    size_t at = sizeof kMagic + sizeof kVersion;
    uint32_t version;
    if (bytes.size() < at || std::memcmp(bytes.data(), kMagic, sizeof kMagic)) state_fail("not a dump");
    std::memcpy(&version, bytes.data() + sizeof kMagic, sizeof version);
    if (version != kVersion) state_fail("a dump in another format's version");
    std::map<std::string, std::vector<uint8_t>> parts;
    while (at < bytes.size()) {
        uint8_t n = bytes[at++];
        uint64_t size;
        if (bytes.size() - at < n + sizeof size) state_fail("cut short");
        std::string name(reinterpret_cast<const char*>(bytes.data() + at), n);
        std::memcpy(&size, bytes.data() + at + n, sizeof size);
        at += n + sizeof size;
        if (bytes.size() - at < size) state_fail("cut short");
        parts[name].assign(bytes.begin() + (std::ptrdiff_t)at, bytes.begin() + (std::ptrdiff_t)(at + size));
        at += size;
    }
    for (auto& p : kParts) {
        auto it = parts.find(p.name);
        if (it == parts.end()) state_fail((std::string("no part ") + p.name).c_str());
        State s;
        s.loading = true;
        s.data = std::move(it->second);
        p.fn(s);
        if (s.pos != s.data.size()) state_fail((std::string("the part ") + p.name + " is longer than this build reads").c_str());
    }
    sat_note("state: loaded %s at VBlank %llu, going on at %08X", path.c_str(), (unsigned long long)sat_vblanks(), g_master.pc);
    g_loading_path.clear();
}

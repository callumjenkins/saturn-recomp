// saturn-recomp runtime — the machine: the run loop, time, interrupts taken by a
// CPU, the slave SH-2, and program starts.
//
// Time. The recompiled code has no cycle counts; it has safe points (loop
// back-edges, calls, `ldc ...,sr`). Every kBudget safe points the master
// polls: time moves on, the devices run, interrupts are taken. Time is
// virtual by default (kNsPerSafePoint per safe point: a run is the same run
// every time) or the host's clock (`realtime`).
//
// Interrupts are taken at a poll, as the CPU would between two instructions:
// SR and a return PC pushed on the guest stack, the mask raised to the
// level, the handler reached through the vector table at VBR. A vector that
// still points at the BIOS's dispatcher goes to the handler SYS_SETUINT
// installed, called like a function (the BIOS saves the registers and does
// the rte); any other is the game's own handler and ends with its rte.
//
// The slave SH-2 is a second context on a host thread used as a coroutine:
// exactly one CPU runs at a time, and control changes hands at fixed points,
// so a run stays deterministic. SSHON boots it (from the address the BIOS
// reads at 0x06000250, vector 0x94, which SYS_SETSINT sets); it runs until it
// waits: it reads its FRT's input-capture flag and finds nothing there (the
// SBL slave loop), or it has used a slice of safe points. A write to SINIT
// sets that flag and hands it the CPU at once; while it has work, the master
// gives it a slice at each of its own polls.
//
// Program starts. A call to a module's base address is a crt0: it resets the
// stack and never returns. The runtime identifies the image in memory,
// activates its module, and throws back to its own loop, which calls the
// entry on a fresh host stack: the old program's C++ frames do not pile up
// under the new one.
#include "saturn.h"
#include "host.h"
#include "video.h"
#include "state.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

SaturnConfig g_cfg;
SH2Context g_master, g_slave;
SH2Context* g_cpu = &g_master;

static const int32_t kBudget = 256;             // safe points between polls
static const uint64_t kNsPerSafePoint = 400;    // virtual time: ~11 instructions at 28.6 MHz
static const uint32_t kIntPC = 0xFFFFFFE4u;     // the PC an interrupt pushes: where rte must go
static const int kSlaveSlice = 64;              // polls the slave runs before it gives the CPU back

struct ProgramStart { uint32_t addr; };
struct RunStop {};
struct SlaveReset {};

static uint64_t g_vtime;                        // virtual ns
static std::chrono::steady_clock::time_point g_t0;
static int g_starts;

uint64_t sat_now() {
    if (g_cfg.realtime)
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - g_t0).count();
    return g_vtime;
}

static void vprint(const char* tag, const char* fmt, va_list ap) {
    std::fprintf(stderr, "[%8.3f %s] ", sat_now() / 1e9, tag);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

void sat_trace(const char* fmt, ...) {
    if (!g_cfg.trace) return;
    va_list ap;
    va_start(ap, fmt);
    vprint(g_cpu->cpu ? "S" : "M", fmt, ap);
    va_end(ap);
}

void sat_note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprint(g_cpu->cpu ? "S" : "M", fmt, ap);
    va_end(ap);
}

// Where a CPU is, roughly: the words on its stack that point into WRAM-H
// code but are not entries (return addresses, mostly).
static void backtrace(const SH2Context& c) {
    std::fprintf(stderr, "  stack (cpu %d, r15 %08X):", c.cpu, c.r[15]);
    int n = 0;
    for (uint32_t a = c.r[15] & ~3u; a < (c.r[15] & ~3u) + 0x400 && n < 24; a += 4) {
        if (!SH2_IS_WRAM_H(a)) break;
        uint32_t v = ld32(a);
        if (v >= 0x06004000u && v < 0x06100000u && !(v & 1) && !sh2_lookup(v)) {
            std::fprintf(stderr, " %08X", v);
            ++n;
        }
    }
    std::fprintf(stderr, "\n");
}

// Where the master has been polling: the host return address of each of the
// last 4096 polls lies in the recompiled function that polled; the modules'
// tables map it back to that function's guest entry.
static void* g_poll_ring[4096];
static unsigned g_poll_pos;

// The recompiled function a host code address lies in: its module and guest entry
struct HostFn { uintptr_t host; uint32_t addr; const char* mod; };
static const HostFn* host_function(const void* p) {
    static std::vector<HostFn> fns;
    if (fns.empty()) {
        for (int i = 0; i < g_sh2_nmodules; ++i)
            for (uint32_t k = 0; k < g_sh2_modules[i]->nfuncs; ++k)
                fns.push_back({(uintptr_t)g_sh2_modules[i]->funcs[k].fn, g_sh2_modules[i]->funcs[k].addr,
                               g_sh2_modules[i]->name});
        std::sort(fns.begin(), fns.end(), [](const HostFn& a, const HostFn& b) { return a.host < b.host; });
    }
    auto it = std::upper_bound(fns.begin(), fns.end(), (uintptr_t)p, [](uintptr_t v, const HostFn& f) { return v < f.host; });
    return it == fns.begin() ? nullptr : &*(it - 1);
}

// --watch over the work RAMs: every store, its value, the function that made it
static void watch_report(uint32_t a, uint32_t v, int size, const void* host) {
    if (sat_vblanks() < g_cfg.watch_from || sat_vblanks() > g_cfg.watch_to) return;
    const HostFn* f = host_function(host);
    sat_note("store%d %08X = %0*X in %s:%08X (pr %08X, VBlank %llu)", size * 8, a, size * 2, v,
             f ? f->mod : "?", f ? f->addr : 0, g_cpu->pr, (unsigned long long)sat_vblanks());
}

static void hot_spots() {
    std::map<std::pair<const char*, uint32_t>, int> hist;
    for (void* p : g_poll_ring) {
        if (!p) continue;
        if (const HostFn* f = host_function(p)) ++hist[{f->mod, f->addr}];
    }
    std::vector<std::pair<int, std::pair<const char*, uint32_t>>> top;
    for (auto& [k, n] : hist) top.push_back({n, k});
    std::sort(top.rbegin(), top.rend());
    std::fprintf(stderr, "  last polls in:");
    for (size_t i = 0; i < top.size() && i < 8; ++i)
        std::fprintf(stderr, " %s:%08X x%d", top[i].second.first, top[i].second.second, top[i].first);
    std::fprintf(stderr, "\n");
}

// --write VBLANK:ADDR=HEX,...: bytes written through the CPU's stores at that VBlank-IN, at the
// point an agent's write would go in
struct Write { uint64_t vblank; uint32_t addr; std::vector<uint8_t> bytes; };
static std::vector<Write> g_writes;
static size_t g_write_pos;

static void parse_writes(const std::string& spec) {
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        std::string item = spec.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? spec.size() : j + 1;
        unsigned long long vblank;
        unsigned addr;
        char hex[1024];
        if (std::sscanf(item.c_str(), "%llu:%x=%1023[0-9A-Fa-f]", &vblank, &addr, hex) != 3 || std::strlen(hex) % 2)
            sat_fatal("--write: %s is not VBLANK:ADDR=HEX", item.c_str());
        Write w{vblank, addr, {}};
        for (size_t k = 0; hex[k]; k += 2) w.bytes.push_back((uint8_t)std::stoul(std::string(hex + k, 2), nullptr, 16));
        g_writes.push_back(w);
    }
    std::stable_sort(g_writes.begin(), g_writes.end(), [](const Write& x, const Write& y) { return x.vblank < y.vblank; });
}

static void writes_due() {
    while (g_write_pos < g_writes.size() && g_writes[g_write_pos].vblank <= sat_vblanks()) {
        const Write& w = g_writes[g_write_pos++];
        for (size_t k = 0; k < w.bytes.size(); ++k) st8(w.addr + (uint32_t)k, w.bytes[k]);
        sat_note("write %08X: %zu bytes", w.addr, w.bytes.size());
    }
}

// --peek ADDR[:WORDS],...: 32-bit words of memory, through the ordinary reads
static void peek(const std::string& spec) {
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        std::string item = spec.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? spec.size() : j + 1;
        uint32_t a = (uint32_t)std::stoul(item, nullptr, 16), n = 1;
        size_t colon = item.find(':');
        if (colon != std::string::npos) n = (uint32_t)std::stoul(item.substr(colon + 1));
        std::fprintf(stderr, "  %08X:", a);
        for (uint32_t k = 0; k < n; ++k) std::fprintf(stderr, " %08X", ld32(a + 4 * k));
        std::fprintf(stderr, "\n");
    }
}

static uint64_t g_ints[0x80];

static void report_ints() {
    std::fprintf(stderr, "  interrupts taken:");
    for (int v = 0; v < 0x80; ++v)
        if (g_ints[v]) std::fprintf(stderr, " %02X:%llu", v, (unsigned long long)g_ints[v]);
    std::fprintf(stderr, "\n");
}

static void write_coverage(const std::string& path);

void sat_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprint("FATAL", fmt, ap);
    va_end(ap);
    SH2Context& c = *g_cpu;
    std::fprintf(stderr, "  cpu %d  pr %08X  pc %08X  sr %03X\n", c.cpu, c.pr, c.pc, sh2_get_sr(c));
    for (int i = 0; i < 16; ++i) std::fprintf(stderr, "  r%-2d %08X%s", i, c.r[i], i % 4 == 3 ? "\n" : "");
    backtrace(c);
    report_ints();
    hot_spots();
    mmio_log_write(g_cfg.out + "/hw-log.txt");
    write_coverage(g_cfg.coverage);
    static bool closing;                        // a fatal error while closing them must not come back here
    if (!closing && (movie_on() || !g_cfg.wav.empty())) {
        closing = true;
        sound_close();                          // a run that ends in an error is the one most worth watching
        movie_finish();
    }
    std::fflush(stderr);
    std::_Exit(1);
}

static const char* g_stop_why;
void sat_stop(const char* why) {
    g_stop_why = why;
    if (g_cpu->cpu) sat_fatal("stop requested on the slave: %s", why);
    throw RunStop{};
}

// ---- the slave: a coroutine on its own thread -----------------------------------------------
static std::mutex g_mu;
static std::condition_variable g_cv;
static int g_turn;                  // 0 the master runs, 1 the slave
static bool g_slave_on, g_slave_idle = true, g_slave_reset, g_slave_thread;
static int g_slave_polls;

static void give(int to) {          // hand the CPU over and wait until it comes back
    std::unique_lock<std::mutex> lk(g_mu);
    g_turn = to;
    g_cv.notify_all();
    g_cv.wait(lk, [&] { return g_turn == 1 - to; });
}

static void to_slave() {
    give(1);
    g_cpu = &g_master;
}

static void to_master() {
    give(0);
    g_cpu = &g_slave;
    g_slave_polls = 0;
    if (g_slave_reset) throw SlaveReset{};
}

static void slave_main() {
    {
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait(lk, [] { return g_turn == 1; });
    }
    g_cpu = &g_slave;
    for (;;) {
        try {
            g_slave_reset = false;
            while (!g_slave_on) { g_slave_idle = true; to_master(); }
            // the BIOS's slave boot: its own vector table, interrupts masked,
            // its stack, then the entry the program set as vector 0x94
            SH2Context& s = g_slave;
            s = SH2Context{};
            s.cpu = 1;
            s.vbr = 0x06000400u;
            s.imask = 15;
            s.r[15] = 0x06001000u;
            s.budget = kBudget;
            uint32_t entry = ld32(0x06000250u);
            sat_trace("slave boots at %08X", entry);
            g_slave_idle = false;
            sh2_call(s, entry);
            sat_fatal("the slave's entry %08X returned", entry);
        } catch (SlaveReset&) {
            sat_trace("slave reset");
        }
    }
}

// ---- the stall report ---------------------------------------------------------------------------
std::atomic<const char*> g_host_call{""};
static std::atomic<bool> g_run_over;           // the run has stopped: what follows (an encoder finishing) is no stall

// Watches a run nothing else steps (no agent) from its own thread: when no VBlank has come for five
// seconds it reports where both CPUs and the host are, and again when the run goes on. A window
// that freezes is otherwise killed with nothing in the log.
static void watch_for_stalls() {
    using namespace std::chrono;
    uint64_t seen = sat_vblanks();
    auto since = steady_clock::now();
    bool stalled = false;
    for (;;) {
        std::this_thread::sleep_for(milliseconds(250));
        if (g_run_over) return;
        uint64_t now = sat_vblanks();
        double still = duration<double>(steady_clock::now() - since).count();
        if (now != seen) {
            if (stalled) sat_note("stall over: VBlank %llu came after %.1f s", (unsigned long long)now, still);
            seen = now, since = steady_clock::now(), stalled = false;
        } else if (!stalled && still >= 5) {
            stalled = true;
            std::fprintf(stderr, "STALL: no VBlank for 5 s after VBlank %llu; running %s, host call \"%s\", turn %s\n",
                         (unsigned long long)now, g_cpu->cpu ? "slave" : "master", g_host_call.load(),
                         g_turn ? "slave" : "master");
            std::fprintf(stderr, "  master pc %08X pr %08X; slave pc %08X pr %08X%s\n", g_master.pc, g_master.pr,
                         g_slave.pc, g_slave.pr, g_slave_on ? "" : " (off)");
            hot_spots();
            std::fflush(stderr);
        }
    }
}

bool state_slave_busy() { return g_slave_on; }

void slave_on() {
    if (!g_slave_thread) {
        g_slave_thread = true;
        std::thread(slave_main).detach();
    }
    if (g_slave_on) return;
    g_slave_on = true;
    onchip_reset(1);
    sat_trace("SSHON");
    to_slave();                     // it boots and runs to its first wait
}

void slave_off() {
    sat_trace("SSHOFF");
    if (!g_slave_on) return;
    g_slave_on = false;
    g_slave_reset = true;
    to_slave();                     // it unwinds and parks
}

void slave_idle_check(uint32_t ftcsr) {
    if (g_cpu->cpu != 1 || (ftcsr & 0x80)) return;
    g_slave_idle = true;
    to_master();
}

// SINIT: the master pulses the slave's FRT input capture
void slave_kick() {
    onchip_input_capture(1);
    if (!g_slave_on) return;
    g_slave_idle = false;
    to_slave();
}

// The clock and the multitap are the run's own settings, but a dump goes on with the ones it was made with.
void machine_state(State& s) {
    uint8_t ip[16];
    std::memcpy(ip, g_wram_h + 0x2020, sizeof ip);     // IP.BIN's product number and version
    s(ip);
    if (s.loading && std::memcmp(ip, g_wram_h + 0x2020, sizeof ip)) state_fail("a dump of another disc");
    if (!s.loading && g_slave_on) state_fail("taken with the slave on");
    s(g_vtime);
    s(g_starts);
    s(g_ints);
    s(g_cfg.clock);
    s(g_cfg.multitap);
    s(g_master);
}

// --coverage FILE: "MODULE ADDRESS INSTRUCTIONS RAN" for every recompiled function, RAN 1 or 0.
// Written whole and then renamed over the last, so a reader never sees half a file.
static void write_coverage(const std::string& path) {
    if (path.empty()) return;
    std::string part = path + ".part";
    FILE* f = std::fopen(part.c_str(), "w");
    if (!f) { sat_note("coverage: cannot write %s", part.c_str()); return; }
    for (int i = 0; i < g_sh2_nmodules; ++i) {
        const SH2Module* m = g_sh2_modules[i];
        for (uint32_t k = 0; k < m->nfuncs; ++k)
            std::fprintf(f, "%s %08X %u %d\n", m->name, m->funcs[k].addr, m->sizes[k], m->ran[k] ? 1 : 0);
    }
    std::fclose(f);
    std::error_code e;
    std::filesystem::rename(part, path, e);
    if (e) sat_note("coverage: cannot replace %s: %s", path.c_str(), e.message().c_str());
}

// --checkpoint SECONDS: coverage written again every SECONDS of the host's time, so a run that is
// killed rather than stopped still leaves what it ran.
static void checkpoint_due() {
    using Clock = std::chrono::steady_clock;
    static uint64_t vblank = ~0ull;
    static Clock::time_point next;
    if (!g_cfg.checkpoint || sat_vblanks() == vblank) return;
    vblank = sat_vblanks();
    Clock::time_point now = Clock::now();
    if (next == Clock::time_point{}) next = now + std::chrono::seconds(g_cfg.checkpoint);
    if (now < next) return;
    next = now + std::chrono::seconds(g_cfg.checkpoint);
    write_coverage(g_cfg.coverage);
    sat_note("checkpoint at VBlank %llu", (unsigned long long)vblank);
}

// ---- time, devices, interrupts ----------------------------------------------------------
void master_poll_devices() {
    uint64_t now = sat_now();
    video_tick(now);
    writes_due();
    agent_poll();
    cd_tick();
    smpc_tick();
    sound_tick();
    checkpoint_due();
    state_poll();
    if (g_slave_on && !g_slave_idle) to_slave();
    if (g_cfg.stop_vblanks && sat_vblanks() >= g_cfg.stop_vblanks) sat_stop("VBlank limit");
}

void sh2_poll(SH2Context& c) {
    c.budget = kBudget;
    if (c.cpu == 1) {
        if (++g_slave_polls >= kSlaveSlice) to_master();
        return;
    }
    g_poll_ring[g_poll_pos++ % 4096] = __builtin_return_address(0);
    g_vtime += kBudget * kNsPerSafePoint;
    master_poll_devices();
    scu_deliver(c);
    onchip_deliver(c);
}

static int g_int_depth;

bool sat_in_interrupt() { return g_int_depth > 0; }

void sat_interrupt(SH2Context& c, uint32_t vec, uint32_t level) {
    struct Depth { Depth() { ++g_int_depth; } ~Depth() { --g_int_depth; } } depth;
    ++g_ints[vec & 0x7F];
    SH2Context saved = c;
    uint32_t target = ld32(c.vbr + vec * 4);
    c.r[15] -= 4; st32(c.r[15], sh2_get_sr(c));
    c.r[15] -= 4; st32(c.r[15], kIntPC);
    c.imask = level;
    if (bios_is_dispatcher(vec, target)) {
        bios_dispatch(c, vec);
    } else {
        c.pc = 0;
        sh2_call(c, target);
        if (c.pc != kIntPC) sat_fatal("interrupt %02X: handler %08X did not rte (went to %08X)", vec, target, c.pc);
    }
    int32_t budget = c.budget;
    c = saved;
    c.budget = budget;
}

void sh2_sleep(SH2Context& c, uint32_t pc) {
    // wait for an interrupt: time passes until one is taken
    for (int i = 0; i < 100000; ++i) {
        g_vtime += kBudget * kNsPerSafePoint;
        master_poll_devices();
        if (scu_deliver(c) | onchip_deliver(c)) return;
    }
    sat_fatal("sleep at %08X: nothing woke it", pc);
}

void sh2_trapa(SH2Context& c, uint32_t imm, uint32_t pc) {
    (void)c;
    sat_fatal("trapa #%u at %08X", imm, pc);
}

void sh2_bad_return(SH2Context& c, uint32_t expected) {
    if (tasks_pending()) { tasks_route(c, expected); return; }
    sat_fatal("returned to %08X, expected %08X", c.pc, expected);
}

void sh2_call_unknown(SH2Context& c, uint32_t addr) {
    if (bios_call(c, addr)) return;
    // Code called inside an image no program start activated, such as IP.BIN's
    bool exact;
    if (const SH2Module* m = sh2_identify_containing(addr, &exact)) {
        sh2_activate(m);
        if (SH2Func f = sh2_lookup(addr)) {
            sat_note("%s activated by a call to %08X%s", m->name, addr, exact ? "" : " (its data differs from the image's)");
            f(c);
            return;
        }
    }
    mmio_log_call(addr, c.pr);
    sat_fatal("call to %08X, not an entry of an active module (from pr %08X)", addr, c.pr);
}

void sh2_program_start(SH2Context& c, uint32_t addr) {
    if (c.cpu != 0) sat_fatal("the slave started a program at %08X", addr);
    const SH2Module* m = sh2_identify(addr);
    if (m) sh2_activate(m);
    else if (!sh2_lookup(addr)) sat_fatal("program start at %08X: no module matches the image there", addr);
    ++g_starts;
    sat_note("program start %d: %s at %08X (r4 %08X, r5 %08X)", g_starts, m ? m->name : "(same module)",
             addr, c.r[4], c.r[5]);
    throw ProgramStart{addr};
}

int saturn_main(const SaturnConfig& cfg) {
    g_cfg = cfg;
    g_t0 = std::chrono::steady_clock::now();
    mmio_watch(cfg.watch_lo, cfg.watch_hi);
    if (cfg.watch_lo <= cfg.watch_hi && (SH2_IS_WRAM_H(cfg.watch_lo) || SH2_IS_WRAM_L(cfg.watch_lo))) {
        extern void (*g_sh2_watch_report)(uint32_t, uint32_t, int, const void*);
        g_sh2_watch_lo = cfg.watch_lo & 0xDFFFFFFFu;
        g_sh2_watch_len = cfg.watch_hi - cfg.watch_lo + 1;
        g_sh2_watch_report = watch_report;
    }
    smpc_input_script(cfg.input);
    parse_writes(cfg.writes);
    if (!cdrom_open(cfg.cue)) { std::fprintf(stderr, "cannot open the disc %s\n", cfg.cue.c_str()); return 2; }
    g_master = SH2Context{};
    g_master.budget = kBudget;
    video_init();
    sound_init();
    cd_init();
    onchip_reset(0);
    bios_boot();
    uint32_t entry = bios_first_read();
    const SH2Module* m = sh2_identify(entry);
    if (!m) { std::fprintf(stderr, "no module matches the 1st read file at %08X\n", entry); return 2; }
    sh2_activate(m);
    sat_note("boot: %s at %08X", m->name, entry);
    if (cfg.task_setjmp) tasks_configure(cfg.task_setjmp, cfg.task_longjmp);
    else if (g_sh2_tasks[0]) tasks_configure(g_sh2_tasks[0], g_sh2_tasks[1]);
    bool tasks = cfg.task_setjmp || g_sh2_tasks[0];
    if (tasks) state_enable();
    state_init();
    if (!cfg.state_in.empty()) {
        state_load(cfg.state_in);
        entry = g_master.pc;
        tasks = true;                           // the dump goes on from one PC, on a task's fresh stack
    }
    if (cfg.agent_fd < 0) std::thread(watch_for_stalls).detach();
    for (;;) {
        try {
            if (tasks) tasks_run(entry);
            SH2Func f = sh2_lookup(entry);
            if (!f) sat_fatal("no function at %08X", entry);
            f(g_master);
            if (!tasks_pending()) sat_fatal("the program at %08X returned", entry);
            tasks_route(g_master, ~0u);
        } catch (TaskUnwind& u) {
            entry = u.pc;
        } catch (ProgramStart& s) {
            tasks_reset();
            entry = s.addr;
            if (cfg.stop_starts && g_starts >= cfg.stop_starts) { g_stop_why = "program-start limit"; break; }
        } catch (RunStop&) {
            break;
        }
    }
    g_run_over = true;
    sat_note("stopped (%s) after %llu VBlanks, %.3f s, %d program starts, %llu VDP1 frame changes, %llu draws",
             g_stop_why ? g_stop_why : "?", (unsigned long long)sat_vblanks(), sat_now() / 1e9, g_starts,
             (unsigned long long)video_frame_changes(), (unsigned long long)video_draws());
    report_ints();
    std::fprintf(stderr, "  master pr %08X, sr %03X\n", g_master.pr, sh2_get_sr(g_master));
    backtrace(g_master);
    hot_spots();
    peek(cfg.peek);
    write_coverage(cfg.coverage);
    sound_close();
    movie_finish();
    mmio_log_write(cfg.out + "/hw-log.txt");
    bios_save();
    state_finish();
    std::fflush(stderr);
    std::fflush(stdout);
    std::_Exit(0);                  // the slave's thread is parked: do not wait for it
}

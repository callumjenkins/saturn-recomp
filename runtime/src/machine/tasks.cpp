// saturn-recomp runtime — a game's own tasks, switched by its setjmp and longjmp.
//
// Some kernels run cooperative tasks, each on its own stack: a task saves its
// registers with setjmp and longjmps into another's saved ones. Recompiled code
// keeps the SH-2 call chain on the host stack, so each task runs here on its
// own host fiber (a minicoro coroutine) on the master's thread, the program's
// first task included (tasks_run); the master's own stack only switches
// between them. The build hooks the first instruction of both functions
// (recomp --hook); `--tasks SETJMP:LONGJMP` tells the runtime which they are.
//
// setjmp records the buffer as the running fiber's, with the r15 and PR it
// saves. longjmp marks a switch to the buffer's fiber. The switch happens where
// longjmp's return lands somewhere other than its caller expected (or in a
// fiber's base loop, after a tail call into longjmp): the running fiber parks
// there and the buffer's fiber resumes, or a new fiber starts at the restored
// PC when no fiber saved that buffer with those registers (a task the kernel
// built by hand). A fiber resumed where it parked returns from that call as if
// nothing happened. A fiber to be resumed anywhere else starts over at the new
// PC on the same stack, which is dropped rather than unwound: it holds only
// recompiled code's frames and the runtime's calls into it, with nothing to
// destroy, and an unwind costs far more than a switch where the unwinder
// parses DWARF on every throw (Android's).
//
// Only the master's tasks are handled. A switch from inside an interrupt handler parks the fiber inside
// it: each fiber keeps its own count of the handlers it is in, and finishes them when it resumes.
#include "saturn.h"
#include "state.h"
#include <exception>
#include <map>
#include <memory>
#include <vector>
#define MINICORO_IMPL
#include "minicoro.h"

namespace {

void fiber_main(mco_coro*);

// The master's own stack has no coroutine: switches between fibers go through it (switch_to).
struct Fiber {
    mco_coro* co = nullptr;
    uint32_t start = 0;
    uint32_t expected = ~0u;                 // where it goes on if resumed where it parked; ~0u: nowhere
    int int_depth = 0;                       // the interrupt handlers it was inside when it parked
    ~Fiber() { if (co) mco_destroy(co); }
};

struct Saved { Fiber* fiber; uint32_t r15, pr; };

Fiber g_main;
Fiber* g_cur = &g_main;
Fiber* g_next;                               // where a fiber that yields asked to go
std::vector<std::unique_ptr<Fiber>> g_fibers;
// jmp_buf address -> who saved it, with what, newest first. A switch inside an interrupt handler can save
// a buffer that the interrupted switch has just saved, and loading it puts the outer save back.
std::map<uint32_t, std::vector<Saved>> g_saved;
const size_t kSavesPerBuffer = 4;

void record(uint32_t buf, const Saved& s) {
    std::vector<Saved>& v = g_saved[buf];
    std::erase_if(v, [&](const Saved& o) { return o.r15 == s.r15 && o.pr == s.pr; });
    v.insert(v.begin(), s);
    if (v.size() > kSavesPerBuffer) v.pop_back();
}
bool g_pending;
uint32_t g_buf;
std::exception_ptr g_exc;                    // raised in a fiber, for the master's loop
void (*g_call)(void*);                       // to run on the thread's stack for the fiber that yielded
void* g_call_arg;
std::exception_ptr g_call_exc;               // raised by g_call, for that fiber
const size_t kStack = 4u << 20;

// minicoro's coroutines only resume and yield, so a fiber yields to the master's stack, which resumes the next.
void switch_to(Fiber* t) {
    if (g_cur != &g_main) {
        g_next = t;
        mco_yield(g_cur->co);
        return;
    }
    while (t != &g_main) {
        g_cur = t;
        if (mco_resume(t->co) != MCO_SUCCESS) sat_fatal("tasks: the fiber from %08X cannot resume", t->start);
        t = g_next;
        if (g_call) {
            g_cur = &g_main;
            try { g_call(g_call_arg); } catch (...) { g_call_exc = std::current_exception(); }
            g_call = nullptr;
        }
    }
    g_cur = &g_main;
    if (g_exc) {
        std::exception_ptr e = g_exc;
        g_exc = nullptr;
        std::rethrow_exception(e);
    }
}

void fiber_main(mco_coro*) {
    Fiber* me = g_cur;
    me->expected = ~0u;
    sat_set_interrupt_depth(0);
    uint32_t pc = me->start;
    try {
        for (;;) {
            try {
                SH2Func f = sh2_lookup(pc);
                if (!f) sat_fatal("task at %08X: no function there", pc);
                f(g_master);
                if (g_pending) tasks_route(g_master, ~0u);
                pc = g_master.pc;
            } catch (TaskUnwind& u) {
                pc = u.pc;
            }
        }
    } catch (...) {
        g_exc = std::current_exception();
        switch_to(&g_main);
    }
}

// Starts `t` over at `pc`, dropping whatever its stack held.
void restart(Fiber* t, uint32_t pc) {
    t->start = pc;
    mco_desc desc = mco_desc_init(fiber_main, kStack);
    if (mco_uninit(t->co) != MCO_SUCCESS || mco_init(t->co, &desc) != MCO_SUCCESS)
        sat_fatal("tasks: the fiber cannot start over at %08X", pc);
}

Fiber* new_fiber(uint32_t pc) {
    auto f = std::make_unique<Fiber>();
    f->start = pc;
    mco_desc desc = mco_desc_init(fiber_main, kStack);
    if (mco_result r = mco_create(&f->co, &desc); r != MCO_SUCCESS)
        sat_fatal("tasks: no fiber for %08X: %s", pc, mco_result_description(r));
    g_fibers.push_back(std::move(f));
    sat_trace("task: a fiber starts at %08X (r15 %08X)", pc, g_master.r[15]);
    return g_fibers.back().get();
}

void on_setjmp(SH2Context& c, uint32_t) {
    if (c.cpu == 0) record(c.r[4], {g_cur, c.r[15], c.pr});
}

void on_longjmp(SH2Context& c, uint32_t) {
    if (c.cpu != 0) sat_fatal("task switch on the slave at %08X: not handled", c.pr);
    g_pending = true;
    g_buf = c.r[4];
}

// Stack-swapping switches (tasks_add_switch), by their save and switch addresses. A switch back into the
// task it saved is routed too: it can return from deeper than it was entered, as a system call's handler
// that takes the switch's way out does, and the router then unwinds the task's fiber to it. Such a switch
// that returns where it was called from leaves its routing pending, until the next save clears it.
std::vector<SH2TaskSwitch> g_switches;

void on_switch_save(SH2Context& c, uint32_t addr) {
    if (c.cpu != 0) return;
    g_pending = false;
    for (const SH2TaskSwitch& s : g_switches)
        if (s.save == addr) record(c.r[s.save_key], {g_cur, c.r[15] + s.resume_sp, c.pr});
}

void on_switch_load(SH2Context& c, uint32_t addr) {
    if (c.cpu != 0) return;
    for (const SH2TaskSwitch& s : g_switches)
        if (s.sw == addr) {
            g_pending = true;
            g_buf = c.r[s.switch_key];
        }
}

}  // namespace

void tasks_add_switch(const SH2TaskSwitch& s) {
    g_switches.push_back(s);
    sh2_hook_add(s.save, on_switch_save);
    sh2_hook_add(s.sw, on_switch_load);
}

void tasks_configure(uint32_t setjmp_addr, uint32_t longjmp_addr) {
    sh2_hook_add(setjmp_addr, on_setjmp);
    sh2_hook_add(longjmp_addr, on_longjmp);
}

bool tasks_pending() { return g_pending; }

void tasks_route(SH2Context& c, uint32_t expected) {
    g_pending = false;
    state_point(c);
    Fiber* t = nullptr;
    if (auto it = g_saved.find(g_buf); it != g_saved.end())
        for (const Saved& o : it->second)
            if (o.r15 == c.r[15] && o.pr == c.pc) { t = o.fiber; break; }
    if (!t) t = new_fiber(c.pc);
    if (t == g_cur) throw TaskUnwind{c.pc};
    if (t->expected != c.pc) restart(t, c.pc);
    g_cur->expected = expected;
    g_cur->int_depth = sat_interrupt_depth();
    switch_to(t);
    sat_set_interrupt_depth(g_cur->int_depth);
}

void tasks_on_thread_stack(void (*fn)(void*), void* arg) {
    if (g_cur == &g_main) return fn(arg);
    g_call = fn;
    g_call_arg = arg;
    g_next = g_cur;
    mco_yield(g_cur->co);
    if (g_call_exc) {
        std::exception_ptr e = g_call_exc;
        g_call_exc = nullptr;
        std::rethrow_exception(e);
    }
}

void tasks_run(uint32_t entry) {
    switch_to(new_fiber(entry));
}

// The saved buffers, with which fiber saved each. A loaded dump has fibers that have run nothing yet:
// each starts over where it is first switched to.
void tasks_state(State& s) {
    std::map<Fiber*, uint32_t> index;
    for (size_t i = 0; i < g_fibers.size(); ++i) index[g_fibers[i].get()] = (uint32_t)i;
    uint32_t nfibers = (uint32_t)g_fibers.size();
    uint64_t n = 0;
    for (auto& [buf, v] : g_saved) n += v.size();
    s(nfibers);
    s(n);
    if (s.loading) {
        if (g_cur != &g_main) state_fail("loaded from a task");
        g_fibers.clear();
        g_saved.clear();
        for (uint32_t i = 0; i < nfibers; ++i) new_fiber(0);
    }
    std::vector<std::pair<uint32_t, Saved>> rows;
    for (auto& [buf, v] : g_saved)
        for (const Saved& o : v) rows.push_back({buf, o});
    for (uint64_t k = 0; k < n; ++k) {
        uint32_t buf = 0, fiber = 0, r15 = 0, pr = 0;
        if (!s.loading) {
            buf = rows[k].first, r15 = rows[k].second.r15, pr = rows[k].second.pr;
            auto f = index.find(rows[k].second.fiber);
            if (f == index.end()) state_fail("a buffer saved outside a task");
            fiber = f->second;
        }
        s(buf), s(fiber), s(r15), s(pr);
        if (!s.loading) continue;
        if (fiber >= nfibers) state_fail("a buffer saved by no task");
        g_saved[buf].push_back({g_fibers[fiber].get(), r15, pr});
    }
}

void tasks_reset() {
    if (g_cur != &g_main) sat_fatal("tasks reset from a task");
    g_saved.clear();
    g_fibers.clear();
    g_pending = false;
}

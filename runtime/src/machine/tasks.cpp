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
// Only the master's tasks are handled, and never from inside an interrupt.
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
    ~Fiber() { if (co) mco_destroy(co); }
};

struct Saved { Fiber* fiber; uint32_t r15, pr; };

Fiber g_main;
Fiber* g_cur = &g_main;
Fiber* g_next;                               // where a fiber that yields asked to go
std::vector<std::unique_ptr<Fiber>> g_fibers;
std::map<uint32_t, Saved> g_saved;          // jmp_buf address -> who saved it, with what
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
    if (c.cpu == 0) g_saved[c.r[4]] = {g_cur, c.r[15], c.pr};
}

void on_longjmp(SH2Context& c, uint32_t) {
    if (c.cpu != 0) sat_fatal("task switch on the slave at %08X: not handled", c.pr);
    g_pending = true;
    g_buf = c.r[4];
}

}  // namespace

void tasks_configure(uint32_t setjmp_addr, uint32_t longjmp_addr) {
    sh2_hook_add(setjmp_addr, on_setjmp);
    sh2_hook_add(longjmp_addr, on_longjmp);
}

bool tasks_pending() { return g_pending; }

void tasks_route(SH2Context& c, uint32_t expected) {
    if (sat_in_interrupt()) sat_fatal("task switch to %08X inside an interrupt: not handled", c.pc);
    g_pending = false;
    state_point(c);
    Fiber* t = nullptr;
    auto it = g_saved.find(g_buf);
    if (it != g_saved.end() && it->second.r15 == c.r[15] && it->second.pr == c.pc) t = it->second.fiber;
    if (!t) t = new_fiber(c.pc);
    if (t == g_cur) throw TaskUnwind{c.pc};
    if (t->expected != c.pc) restart(t, c.pc);
    g_cur->expected = expected;
    switch_to(t);
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
    uint64_t n = g_saved.size();
    s(nfibers);
    s(n);
    if (s.loading) {
        if (g_cur != &g_main) state_fail("loaded from a task");
        g_fibers.clear();
        g_saved.clear();
        for (uint32_t i = 0; i < nfibers; ++i) new_fiber(0);
    }
    auto it = g_saved.begin();
    for (uint64_t k = 0; k < n; ++k) {
        uint32_t buf = 0, fiber = 0, r15 = 0, pr = 0;
        if (!s.loading) {
            buf = it->first, r15 = it->second.r15, pr = it->second.pr;
            auto f = index.find(it->second.fiber);
            if (f == index.end()) state_fail("a buffer saved outside a task");
            fiber = f->second;
            ++it;
        }
        s(buf), s(fiber), s(r15), s(pr);
        if (!s.loading) continue;
        if (fiber >= nfibers) state_fail("a buffer saved by no task");
        g_saved[buf] = {g_fibers[fiber].get(), r15, pr};
    }
}

void tasks_reset() {
    if (g_cur != &g_main) sat_fatal("tasks reset from a task");
    g_saved.clear();
    g_fibers.clear();
    g_pending = false;
}

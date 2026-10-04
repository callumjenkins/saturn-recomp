// saturn-recomp runtime — a game's own tasks, switched by its setjmp and longjmp.
//
// Some kernels run cooperative tasks, each on its own stack: a task saves its
// registers with setjmp and longjmps into another's saved ones. Recompiled code
// keeps the SH-2 call chain on the host stack, so each task runs here on its
// own host fiber (ucontext) on the master's thread. The build hooks the first
// instruction of both functions (recomp --hook); `--tasks SETJMP:LONGJMP`
// tells the runtime which they are.
//
// setjmp records the buffer as the running fiber's, with the r15 and PR it
// saves. longjmp marks a switch to the buffer's fiber. The switch happens where
// longjmp's return lands somewhere other than its caller expected (or in a
// fiber's base loop, after a tail call into longjmp): the running fiber parks
// there and the buffer's fiber resumes, or a new fiber starts at the restored
// PC when no fiber saved that buffer with those registers (a task the kernel
// built by hand). A fiber resumed where it parked returns from that call as if
// nothing happened; resumed anywhere else, its host stack unwinds to its base
// loop (TaskUnwind), which goes on at the new PC. The master's base loop is
// saturn_main's.
//
// Only the master's tasks are handled, and never from inside an interrupt.
#include "saturn.h"
#include <exception>
#include <map>
#include <memory>
#include <ucontext.h>
#include <vector>

namespace {

struct Fiber {
    ucontext_t ctx;
    std::vector<char> stack;
    uint32_t start = 0;
};

struct Saved { Fiber* fiber; uint32_t r15, pr; };

Fiber g_main;
Fiber* g_cur = &g_main;
std::vector<std::unique_ptr<Fiber>> g_fibers;
std::map<uint32_t, Saved> g_saved;          // jmp_buf address -> who saved it, with what
bool g_pending;
uint32_t g_buf;
std::exception_ptr g_exc;                    // raised in a fiber, for the master's loop
const size_t kStack = 4u << 20;

void switch_to(Fiber* t) {
    Fiber* me = g_cur;
    g_cur = t;
    swapcontext(&me->ctx, &t->ctx);
    if (me == &g_main && g_exc) {
        std::exception_ptr e = g_exc;
        g_exc = nullptr;
        std::rethrow_exception(e);
    }
}

void fiber_main() {
    Fiber* me = g_cur;
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

Fiber* new_fiber(uint32_t pc) {
    auto f = std::make_unique<Fiber>();
    f->stack.resize(kStack);
    f->start = pc;
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack.data();
    f->ctx.uc_stack.ss_size = f->stack.size();
    f->ctx.uc_link = nullptr;
    makecontext(&f->ctx, fiber_main, 0);
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
    Fiber* t = nullptr;
    auto it = g_saved.find(g_buf);
    if (it != g_saved.end() && it->second.r15 == c.r[15] && it->second.pr == c.pc) t = it->second.fiber;
    if (!t) t = new_fiber(c.pc);
    if (t == g_cur) throw TaskUnwind{c.pc};
    switch_to(t);
    if (c.pc == expected) return;
    throw TaskUnwind{c.pc};
}

void tasks_reset() {
    if (g_cur != &g_main) sat_fatal("tasks reset from a task");
    g_saved.clear();
    g_fibers.clear();
    g_pending = false;
}

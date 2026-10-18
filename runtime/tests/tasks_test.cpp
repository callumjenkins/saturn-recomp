// The game's tasks: each on its own fiber, switched by its setjmp and longjmp, and an exception
// thrown on one reaching the program's loop on the thread's own stack.
#include "saturn.h"
#include "test.h"
#include <string>

namespace {

const uint32_t SETJMP = 0x06003000, LONGJMP = 0x06003100;
const uint32_t A = 0x06004000, A_RESUME = 0x06004100, A_OTHER = 0x06004200, B = 0x06005000, C = 0x06006000;
const uint32_t BUF_A = 0x060F0000, BUF_B = 0x060F0100;

struct Done { int n; };                     // stands in for a program start, which ends the tasks
std::string g_log;
uint32_t g_back = A_RESUME;                 // where A's buffer says it goes on, and B sends it

// Task A saves its buffer, as setjmp would, and switches to B, which has none yet. A parks expecting to go on
// at A_RESUME.
void task_a(SH2Context& c) {
    g_log += "a1 ";
    c.r[4] = BUF_A, c.r[15] = 0x06001000, c.pr = g_back;
    sh2_hook(c, SETJMP);
    sat_set_interrupt_depth(1);
    c.r[4] = BUF_B;
    sh2_hook(c, LONGJMP);
    c.r[15] = 0x06002000, c.pc = B;
    tasks_route(c, A_RESUME);
    g_log += "a2:" + std::to_string(sat_interrupt_depth()) + " ";
    throw Done{1};
}

// Where task A goes on when its buffer leads somewhere other than where it parked.
void task_a_other(SH2Context&) {
    g_log += "a-other ";
    throw Done{2};
}

// Task B longjmps to A's buffer.
void task_b(SH2Context& c) {
    g_log += "b1:" + std::to_string(sat_interrupt_depth()) + " ";
    c.r[4] = BUF_A;
    sh2_hook(c, LONGJMP);
    c.r[15] = 0x06001000, c.pc = g_back;
    tasks_route(c, ~0u);
    g_log += "b2 ";
}

void task_c(SH2Context&) {
    g_log += "c ";
    throw Done{3};
}

const SH2FuncEntry kFuncs[] = {{A, task_a}, {A_RESUME, task_a}, {A_OTHER, task_a_other}, {B, task_b}, {C, task_c}};
const SH2Module kModule{"tasks-test", 0x06003000, 0x4000, 0, kFuncs, 5, nullptr, nullptr};

// The program from `entry` as the first task, until it throws Done: Done's n, or 0 if it ends otherwise.
int run(uint32_t entry) {
    static bool configured;
    if (!configured) {
        sh2_activate(&kModule);
        tasks_configure(SETJMP, LONGJMP);
        configured = true;
    }
    g_log.clear();
    sat_set_interrupt_depth(0);
    int n = 0;
    try {
        tasks_run(entry);
    } catch (Done& d) {
        n = d.n;
    }
    tasks_reset();
    return n;
}

}  // namespace

TEST(a_task_that_yields_resumes_where_it_parked) {
    g_back = A_RESUME;
    CHECK_EQ(run(A), 1);
    CHECK_EQ(g_log == "a1 b1:0 a2:1 ", true);
}

TEST(a_task_sent_elsewhere_starts_over_there) {
    g_back = A_OTHER;
    CHECK_EQ(run(A), 2);
    CHECK_EQ(g_log == "a1 b1:0 a-other ", true);
}

TEST(an_exception_on_a_fiber_reaches_tasks_runs_caller_again_and_again) {
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(run(C), 3);
        CHECK_EQ(g_log == "c ", true);
    }
}

TEST(tasks_start_afresh_after_a_reset) {
    g_back = A_RESUME;
    CHECK_EQ(run(A), 1);
    CHECK_EQ(run(C), 3);
    CHECK_EQ(g_log == "c ", true);
    CHECK_EQ(run(A), 1);
    CHECK_EQ(g_log == "a1 b1:0 a2:1 ", true);
}

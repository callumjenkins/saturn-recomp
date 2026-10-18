#include "test.h"
#include <filesystem>
#include <random>

static Test* g_tests;
static int g_failures;

Test::Test(const char* n, void (*f)()) : name(n), fn(f), next(g_tests) { g_tests = this; }

void check_failed(const char* file, int line, const char* expr, uint64_t got, uint64_t want) {
    std::fprintf(stderr, "  %s:%d: %s is %llX, expected %llX\n", file, line, expr,
                 (unsigned long long)got, (unsigned long long)want);
    ++g_failures;
}

std::string test_dir(const char* name) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::filesystem::path p = std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(rng()));
    std::filesystem::create_directories(p);
    return p.string();
}

int main() {
    int failed = 0, total = 0;
    for (Test* t = g_tests; t; t = t->next) {
        int before = g_failures;
        t->fn();
        ++total;
        bool ok = g_failures == before;
        failed += !ok;
        std::printf("%-6s %s\n", ok ? "ok" : "FAIL", t->name);
    }
    std::printf("%d tests, %d failed\n", total, failed);
    return failed ? 1 : 0;
}

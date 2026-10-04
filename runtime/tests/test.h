// A test is a function registered with TEST; CHECK_EQ records a failure and carries on.
#pragma once
#include <cstdint>
#include <cstdio>

struct Test {
    const char* name;
    void (*fn)();
    Test* next;
    Test(const char* n, void (*f)());
};
void check_failed(const char* file, int line, const char* expr, uint64_t got, uint64_t want);

#define TEST(name)                                       \
    static void name();                                  \
    static Test name##_registered(#name, name);          \
    static void name()

#define CHECK_EQ(got, want)                                                                      \
    do {                                                                                         \
        uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want);                                    \
        if (g_ != w_) check_failed(__FILE__, __LINE__, #got, g_, w_);                            \
    } while (0)

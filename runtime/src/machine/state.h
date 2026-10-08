// saturn-recomp runtime — the machine dumped to a file and loaded back (state.cpp).
//
// Each part of the machine has one function that writes its fields to a State and, when the State is
// loading, reads them back in the same order, so the two directions cannot drift apart.
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

[[noreturn]] void state_fail(const char* what);

struct State {
    bool loading = false;
    std::vector<uint8_t> data;
    size_t pos = 0;

    void raw(void* p, size_t n) {
        if (!loading) {
            auto* b = static_cast<const uint8_t*>(p);
            data.insert(data.end(), b, b + n);
            return;
        }
        if (n > data.size() - pos) state_fail("a part is shorter than this build reads");
        std::memcpy(p, data.data() + pos, n);
        pos += n;
    }
    template <class T> void operator()(T& v) {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
        raw(&v, sizeof v);
    }
    template <class T> void vec(std::vector<T>& v) {
        uint64_t n = v.size();
        (*this)(n);
        if (loading) v.resize(n);
        if (n) raw(v.data(), n * sizeof(T));
    }
    template <class T> void deq(std::deque<T>& d) {
        std::vector<T> v(d.begin(), d.end());
        vec(v);
        if (loading) d.assign(v.begin(), v.end());
    }
    void str(std::string& s) {
        uint64_t n = s.size();
        (*this)(n);
        if (loading) s.resize(n);
        if (n) raw(s.data(), n);
    }
};

// The parts, each in the file that owns the state.
void machine_state(State& s);
void core_state(State& s);
void mmio_state(State& s);
void bios_state(State& s);
void scu_state(State& s);
void scudsp_state(State& s);
void onchip_state(State& s);
void smpc_state(State& s);
void cdblock_state(State& s);
void video_state(State& s);
void vdp1_state(State& s);
void vdp2_state(State& s);
void sound_state(State& s);
void scsp_state(State& s);
void tasks_state(State& s);

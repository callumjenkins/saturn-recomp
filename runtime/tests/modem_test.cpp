// The NetLink modem through its UART registers, as the software drives it: detection, AT commands, a call
// between two modems, the escape to command mode and a hang-up.
#include "modem.h"
#include "test.h"
#include <deque>
#include <memory>
#include <string>

namespace {

const uint64_t kMs = 1000000;
enum { kData = 0, kLcr = 3, kLsr = 5, kMsr = 6, kScratch = 7 };

// Two ends of one phone line, held in memory.
struct Exchange {
    std::deque<uint8_t> to[2];
    bool ringing = false, up = false, picked = false;
};

struct End : ModemLine {
    std::shared_ptr<Exchange> x;
    int me;
    bool lease = false;
    End(std::shared_ptr<Exchange> x, int me) : x(std::move(x)), me(me) {}
    bool dial(const std::string&) override { x->ringing = true; x->up = true; x->picked = false; return true; }
    bool answered() override { return x->picked; }
    bool ringing() override { return me == 1 && x->ringing; }
    bool answer() override { x->ringing = false; x->picked = x->up; return x->up; }
    void hang_up() override { x->up = x->ringing = x->picked = false; }
    bool connected() override { return x->up; }
    void send(uint8_t b) override { x->to[1 - me].push_back(b); }
    bool receive(uint8_t& b) override {
        if (x->to[me].empty()) return false;
        b = x->to[me].front();
        x->to[me].pop_front();
        return true;
    }
    bool leased() override { return lease; }
};

uint64_t g_now = 1000 * kMs;

void type(Modem& m, const std::string& s) {
    for (char c : s) m.write(kData, (uint8_t)c, g_now);
}

// What the modem has sent by the time `ms` more milliseconds have gone, read as the software polls.
std::string heard(Modem& m, uint64_t ms = 100) {
    std::string out;
    for (uint64_t end = g_now + ms * kMs; g_now < end; g_now += kMs / 4)
        while (m.read(kLsr, g_now) & 1) out += (char)m.read(kData, g_now);
    return out;
}

}  // namespace

TEST(the_scratch_register_reads_back_so_the_modem_is_found) {
    Modem m(modem_no_line());
    m.write(kScratch, 0x22, g_now);
    CHECK_EQ(m.read(kScratch, g_now), 0x22);
    m.write(kScratch, 0x33, g_now);
    CHECK_EQ(m.read(kScratch, g_now), 0x33);
    CHECK_EQ(m.read(kLsr, g_now) & 0x60, 0x60);             // ready to send
    CHECK_EQ(m.read(kMsr, g_now) & 0xB0, 0x30);             // ready, no carrier
}

TEST(an_at_command_is_echoed_and_answered_ok) {
    Modem m(modem_no_line());
    type(m, "AT\r");
    CHECK_EQ(heard(m) == "AT\r\r\nOK\r\n", true);
    type(m, "AT &D0&Q6&S1E0V1&C1W2L3M1s7=30s8=1\\N\r");      // H2H's set-up, which also turns the echo off
    CHECK_EQ(heard(m).find("\r\nOK\r\n") != std::string::npos, true);
    type(m, "AT\r");
    CHECK_EQ(heard(m) == "\r\nOK\r\n", true);
}

TEST(the_answer_arrives_at_the_baud_rate) {
    Modem m(modem_no_line());
    m.write(kLcr, 0x83, g_now);                              // 8N1, and the divisor latch
    m.write(0, 48, g_now);                                   // 2400 baud: a byte each 4.17 ms
    m.write(1, 0, g_now);
    m.write(kLcr, 0x03, g_now);
    type(m, "ATE0\r");
    std::string first = heard(m, 10);
    CHECK_EQ(first.size(), 2u);                              // two bytes in 10 ms
    CHECK_EQ((first + heard(m, 100)) == "ATE0\r\r\nOK\r\n", true);
}

TEST(a_dial_with_nothing_on_the_line_gets_no_carrier) {
    Modem m(modem_no_line());
    type(m, "ATE0\r");
    heard(m);
    type(m, "atdt 5551234\r");
    CHECK_EQ(heard(m) == "\r\nNO CARRIER\r\n", true);
    CHECK_EQ(m.online(), false);
}

TEST(a_call_rings_is_answered_and_carries_bytes_both_ways) {
    auto x = std::make_shared<Exchange>();
    Modem caller(std::make_unique<End>(x, 0)), callee(std::make_unique<End>(x, 1));
    type(caller, "ATE0\r");
    type(callee, "ATE0\r");
    heard(caller);
    heard(callee);
    type(caller, "atdt 5551234\r");
    CHECK_EQ(heard(callee) == "\r\nRING\r\n", true);
    CHECK_EQ(callee.read(kMsr, g_now) & 0x40, 0x40);
    type(callee, "ATA\r");
    CHECK_EQ(heard(callee) == "\r\nCONNECT 28800\r\n", true);
    CHECK_EQ(heard(caller) == "\r\nCONNECT 28800\r\n", true);
    CHECK_EQ(caller.read(kMsr, g_now) & 0x88, 0x88);         // carrier, and that it changed
    CHECK_EQ(caller.read(kMsr, g_now) & 0x88, 0x80);
    type(caller, "BOMB");
    CHECK_EQ(heard(callee) == "BOMB", true);
    type(callee, "MAN");
    CHECK_EQ(heard(caller) == "MAN", true);
}

TEST(plus_plus_plus_between_silences_holds_the_call_and_hanging_up_drops_it) {
    auto x = std::make_shared<Exchange>();
    Modem caller(std::make_unique<End>(x, 0)), callee(std::make_unique<End>(x, 1));
    type(callee, "ATE0S0=1\r");                              // answer on the first ring
    heard(callee);
    type(caller, "ATE0\r");
    heard(caller);
    type(caller, "ATD1\r");
    heard(callee);
    CHECK_EQ(heard(caller) == "\r\nCONNECT 28800\r\n", true);
    heard(caller, 1100);
    type(caller, "+++");
    CHECK_EQ(heard(caller, 1100) == "\r\nOK\r\n", true);
    CHECK_EQ(caller.online(), true);
    type(caller, "ATH\r");
    CHECK_EQ(heard(caller) == "\r\nOK\r\n", true);
    CHECK_EQ(heard(callee).find("NO CARRIER") != std::string::npos, true);
    CHECK_EQ(callee.read(kMsr, g_now) & 0x80, 0);
}

TEST(a_factory_reset_ends_a_call_held_in_command_mode_so_the_next_dial_goes_through) {
    auto x = std::make_shared<Exchange>();
    Modem caller(std::make_unique<End>(x, 0)), callee(std::make_unique<End>(x, 1));
    type(callee, "ATE0S0=1\r");
    heard(callee);
    type(caller, "ATE0\r");
    heard(caller);
    type(caller, "ATD1\r");
    heard(callee);
    heard(caller);
    heard(caller, 1100);
    type(caller, "+++");
    heard(caller, 1100);
    type(caller, "AT&F0\r");
    CHECK_EQ(caller.online(), false);
    CHECK_EQ(heard(callee).find("NO CARRIER") != std::string::npos, true);
    type(caller, "ATD1\r");
    heard(callee);
    CHECK_EQ(heard(caller).find("CONNECT 28800") != std::string::npos, true);
}

TEST(a_leased_line_is_online_from_the_start_without_a_word) {
    auto x = std::make_shared<Exchange>();
    auto a = std::make_unique<End>(x, 0), b = std::make_unique<End>(x, 1);
    a->lease = b->lease = true;
    Modem caller(std::move(a)), callee(std::move(b));
    caller.read(kMsr, g_now);                                // dials
    CHECK_EQ(callee.read(kMsr, g_now) & 0x80, 0x80);         // answers
    CHECK_EQ(caller.read(kMsr, g_now) & 0x80, 0x80);         // hears it
    CHECK_EQ(heard(caller) == "", true);
    type(caller, "H2H");
    CHECK_EQ(heard(callee) == "H2H", true);
}

TEST(the_uart_interrupts_for_received_bytes_and_an_empty_transmitter_while_out2_is_set) {
    Modem m(modem_no_line());
    m.write(4, 0x0B, g_now);                                 // DTR, RTS, OUT2
    m.write(1, 0x01, g_now);                                 // received-data interrupt only
    CHECK_EQ(m.interrupting(g_now), false);
    type(m, "AT\r");
    g_now += 50 * kMs;
    CHECK_EQ(m.interrupting(g_now), true);
    CHECK_EQ(m.read(2, g_now) & 0x0F, 0x04);
    heard(m);
    CHECK_EQ(m.interrupting(g_now), false);
    m.write(1, 0x03, g_now);                                 // and the transmitter's
    CHECK_EQ(m.read(2, g_now) & 0x0F, 0x02);                 // reading IIR clears it
    CHECK_EQ(m.interrupting(g_now), false);
    m.write(4, 0x03, g_now);                                 // OUT2 off: the line stays low
    m.write(0, 'x', g_now);
    CHECK_EQ(m.interrupting(g_now), false);
}

TEST(a_dialled_call_waits_for_an_answer_and_gives_up_after_s7_seconds) {
    auto x = std::make_shared<Exchange>();
    Modem caller(std::make_unique<End>(x, 0)), callee(std::make_unique<End>(x, 1));
    type(caller, "ATE0S7=2\r");
    heard(caller);
    type(callee, "ATE0\r");
    heard(callee);
    type(caller, "ATD1\r");
    CHECK_EQ(heard(caller, 1000) == "", true);               // ringing: nothing to say yet
    CHECK_EQ(heard(callee).find("RING") != std::string::npos, true);
    CHECK_EQ(heard(caller, 1500) == "\r\nNO CARRIER\r\n", true);
    CHECK_EQ(caller.online(), false);
}

// A session's modem line (modem_session.h) without the modems: two lines whose messages wait until delivered, so
// a test can cross them as two peers' polls do.
#include "modem_session.h"
#include "test.h"
#include <deque>
#include <memory>
#include <vector>

namespace {

struct Lines {
    std::deque<std::vector<uint8_t>> to[2];
    std::unique_ptr<SessionLine> line[2];
    Lines() {
        for (int i = 0; i < 2; ++i) {
            line[i] = std::make_unique<SessionLine>([this, i](std::vector<uint8_t> m) { to[1 - i].push_back(std::move(m)); });
            line[i]->reachable(true);
        }
    }
    SessionLine& operator[](int i) { return *line[i]; }
    void deliver() {
        for (int i = 0; i < 2; ++i)
            for (; !to[i].empty(); to[i].pop_front()) line[i]->heard(to[i].front());
    }
    void call() {
        line[0]->dial("1");
        deliver();
        line[1]->answer();
        deliver();
    }
    std::vector<uint8_t> received(int i) {
        std::vector<uint8_t> out;
        for (uint8_t b; line[i]->receive(b);) out.push_back(b);
        return out;
    }
};

}  // namespace

TEST(a_session_dial_hung_up_before_it_is_answered_stops_the_ringing) {
    Lines l;
    l[0].dial("1");
    l.deliver();
    CHECK_EQ(l[1].ringing(), true);
    l[0].hang_up();
    l.deliver();
    CHECK_EQ(l[1].ringing(), false);
    CHECK_EQ(l[1].answer(), false);
}

TEST(a_session_answer_that_crosses_the_hang_up_connects_neither_end) {
    Lines l;
    l[0].dial("1");
    l.deliver();
    l[1].answer();
    l[0].hang_up();
    l.deliver();
    CHECK_EQ(l[0].answered(), false);
    CHECK_EQ(l[0].connected(), false);
    CHECK_EQ(l[1].connected(), false);
}

TEST(a_session_dial_from_both_ends_at_once_rings_neither) {
    Lines l;
    l[0].dial("1");
    l[1].dial("1");
    l.deliver();
    CHECK_EQ(l[0].ringing(), false);
    CHECK_EQ(l[1].ringing(), false);
}

TEST(a_session_hang_up_sends_the_bytes_before_it_and_none_after) {
    Lines l;
    l.call();
    l[0].send('A');
    l[0].hang_up();
    l[0].send('B');
    l[0].flush();
    l.deliver();
    CHECK_EQ(l.received(1) == std::vector<uint8_t>{'A'}, true);
    CHECK_EQ(l[1].connected(), false);
}

TEST(a_session_line_drops_bytes_that_come_after_the_call) {
    Lines l;
    l.call();
    l[1].heard({SessionLine::kHangUp});
    l[1].heard({SessionLine::kData, 'B'});
    CHECK_EQ(l.received(1).empty(), true);
}

TEST(a_session_dial_forgets_the_bytes_left_unread_from_the_last_call) {
    Lines l;
    l.call();
    l[0].send('A');
    l[0].hang_up();
    l.deliver();
    l[1].dial("1");
    CHECK_EQ(l.received(1).empty(), true);
}

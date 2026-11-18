// saturn-recomp runtime — a modem's phone line to the other peer in a session (modem_session.h).
#include "modem_session.h"

void SessionLine::heard(const std::vector<uint8_t>& message) {
    if (message.empty()) return;
    switch (message[0]) {
    case kRing:
        // Both ends dialling at once: neither rings, and both dials go unanswered.
        if (!calling_ && !up_) ringing_ = true;
        break;
    case kAnswer:
        if (!calling_) break;
        calling_ = false;
        up_ = true;
        break;
    case kHangUp: ended(); break;
    case kData:
        if (up_) in_.insert(in_.end(), message.begin() + 1, message.end());
        break;
    }
}

void SessionLine::reachable(bool yes) {
    // A call dialled before the peer could hear it rings now.
    if (yes && !reachable_ && calling_) say(kRing);
    reachable_ = yes;
    if (!yes) ended();
}

void SessionLine::flush() {
    if (out_.empty()) return;
    std::vector<uint8_t> m{kData};
    m.insert(m.end(), out_.begin(), out_.end());
    out_.clear();
    send_(std::move(m));
}

bool SessionLine::dial(const std::string&) {
    if (calling_ || up_) return false;
    ringing_ = false;
    in_.clear();
    calling_ = dialled_ = true;
    if (reachable_) say(kRing);
    return true;
}

bool SessionLine::answer() {
    if (!ringing_) return false;
    ringing_ = false;
    up_ = true;
    dialled_ = false;
    say(kAnswer);
    return true;
}

void SessionLine::hang_up() {
    if (calling_ || up_) {
        flush();
        say(kHangUp);
    }
    ended();
    in_.clear();
}

void SessionLine::send(uint8_t b) {
    if (up_) out_.push_back(b);
}

bool SessionLine::receive(uint8_t& b) {
    if (in_.empty()) return false;
    b = in_.front();
    in_.pop_front();
    return true;
}

void SessionLine::say(uint8_t kind) { send_({kind}); }

void SessionLine::ended() {
    calling_ = ringing_ = up_ = false;
    out_.clear();
}

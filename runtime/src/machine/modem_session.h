// saturn-recomp runtime — a modem's phone line to the other peer in a session (net/session.h): a dial rings that
// peer whatever the number, once its channels are open, and the call's bytes go down the session's line channel.
#pragma once
#include "modem.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

// Each message on the line channel is a kind byte, then a call's data for kData.
class SessionLine : public ModemLine {
public:
    enum : uint8_t { kRing = 'R', kAnswer = 'A', kHangUp = 'H', kData = 'D' };

    // `send` puts a message on the line channel to the other peer.
    explicit SessionLine(std::function<void(std::vector<uint8_t>)> send) : send_(std::move(send)) {}

    void heard(const std::vector<uint8_t>& message);   // a message from the other peer's line
    void reachable(bool yes);                         // the other peer's channels are open, or it has gone
    void flush();                                     // the bytes sent since the last flush, as one message

    bool dial(const std::string& number) override;
    bool answered() override { return up_ && dialled_; }
    bool ringing() override { return ringing_; }
    bool answer() override;
    void hang_up() override;
    bool connected() override { return calling_ || up_ || !in_.empty(); }   // the far end's last bytes, then no carrier
    void send(uint8_t b) override;
    bool receive(uint8_t& b) override;

private:
    void say(uint8_t kind);
    void ended();                // the call is over; what arrived before the far end hung up can still be read

    std::function<void(std::vector<uint8_t>)> send_;
    bool reachable_ = false;
    bool calling_ = false;       // dialled, and not answered yet
    bool dialled_ = false;       // this end made the call
    bool ringing_ = false;       // the other end has dialled and waits for an answer
    bool up_ = false;            // the call is connected
    std::deque<uint8_t> in_;
    std::vector<uint8_t> out_;
};

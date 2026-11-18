// saturn-recomp runtime — a session in a build without network play (SATURN_NO_NET): the room never opens.
#include "session.h"
#include <utility>

struct Session::Impl {
    std::vector<SessionEvent> events;
};

Session::Session(const std::string&, const SessionHello&, std::chrono::milliseconds) : impl_(std::make_unique<Impl>()) {
    impl_->events.push_back({.kind = SessionEvent::Kind::RoomClosed, .text = "this build has no network play"});
}

Session::~Session() = default;

std::vector<SessionEvent> Session::poll() { return std::exchange(impl_->events, {}); }

void Session::send(const std::string&, Channel, std::vector<uint8_t>) {}

void Session::sever() {}

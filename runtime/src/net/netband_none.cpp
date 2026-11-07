// saturn-recomp runtime — the NetBand tunnel in a build without network play (SATURN_NO_NET): every call fails.
#include "netband.h"
#include <utility>

struct NetBand::Impl {
    std::vector<NetBandEvent> events;
};

NetBand::NetBand(NetBandSpec, NetBandServers) : impl_(std::make_unique<Impl>()) {}

NetBand::~NetBand() = default;

void NetBand::wait() { call(); }

void NetBand::call() {
    impl_->events.push_back({.kind = NetBandEvent::Kind::Ended, .text = "this build has no network play"});
}

void NetBand::answer() {}

void NetBand::hang_up() {}

void NetBand::send(const std::vector<uint8_t>&) {}

std::vector<NetBandEvent> NetBand::poll() { return std::exchange(impl_->events, {}); }

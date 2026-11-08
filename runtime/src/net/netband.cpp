// saturn-recomp runtime — a NetLink call through the NetBand tunnel (netband.h). The thread waits, dials, or carries a
// call: a direct caller announces itself with "readyip" on the waiting end's TCP port and hears "g2gip"; through the
// match service, the waiter registers the address STUN gives it and polls its status, and the caller asks for a
// match. Either way the two ends then trade datagrams between the waiter's and the caller's UDP ports.
#include "netband.h"
#include "http_get.h"
#include "netband_sockets.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

const milliseconds kStunEvery(5000);     // the waiter's address, looked up again in case its mapping moved
const milliseconds kStatusEvery(1000);
const milliseconds kPingEvery(1000);
// The tunnel reads 1024 bytes of each datagram, which holds five payloads and their framing.
const size_t kMostPerPayload = 160;

}  // namespace

struct NetBand::Impl {
    enum class State { Idle, Waiting, Calling, Up };
    enum class Ask { Wait, Call, Answer, HangUp };

    NetBandSpec spec;
    NetBandServers servers;

    std::mutex m;   // guards asks, outgoing and events, which the emulation's thread shares
    std::vector<Ask> asks;
    std::vector<uint8_t> outgoing;
    std::vector<NetBandEvent> events;
    std::atomic<bool> stopping{false};
    std::thread thread;

    State state = State::Idle;
    Socket udp = kNoSocket, listener = kNoSocket;
    sockaddr_in far_addr{};
    std::string ip12;          // this end, as the match service knows it
    int mapped_port = 0;
    bool registered = false;
    bool stun_unanswered = false;   // and the waiter has said so
    bool answered = false;
    std::vector<uint8_t> held;   // what the call brought before it was answered
    NetBandSender sender;
    NetBandReceiver receiver;
    Clock::time_point last_stun, last_status, last_ping, last_heard;
    std::mt19937 rng{std::random_device{}()};

    void emit(NetBandEvent e) {
        std::lock_guard lock(m);
        events.push_back(std::move(e));
    }
    void note(const std::string& text) { emit({.kind = NetBandEvent::Kind::Note, .text = "netband: " + text}); }

    void run() {
        while (!stopping) {
            std::vector<Ask> todo;
            {
                std::lock_guard lock(m);
                todo.swap(asks);
            }
            for (Ask a : todo) act(a);
            switch (state) {
            case State::Idle: std::this_thread::sleep_for(milliseconds(10)); break;
            case State::Waiting: keep_waiting(); break;
            case State::Calling: make_call(); break;
            case State::Up: carry(); break;
            }
        }
        stop();
    }

    void act(Ask a) {
        switch (a) {
        case Ask::Wait:
            if (state == State::Idle && spec.wait) start_waiting();
            break;
        case Ask::Call:
            if (state == State::Idle) state = State::Calling;
            break;
        case Ask::Answer:
            if (state != State::Up || answered) break;
            answered = true;
            if (!held.empty()) emit({.kind = NetBandEvent::Kind::Data, .data = std::exchange(held, {})});
            break;
        case Ask::HangUp: stop(); break;
        }
    }

    bool open_udp(int port) {
        if (udp == kNoSocket) udp = netband_udp(port);
        return udp != kNoSocket;
    }

    void start_waiting() {
        listener = netband_listener(servers.direct_port);
        if (listener == kNoSocket) {
            fail("TCP port " + std::to_string(servers.direct_port) + " is in use, so no call can come");
            return;
        }
        if (!spec.code.empty() && !open_udp(servers.wait_port)) {
            fail("UDP port " + std::to_string(servers.wait_port) + " is in use");
            return;
        }
        state = State::Waiting;
        stun_unanswered = false;
        last_stun = last_status = Clock::time_point{};
        note("waiting for a call on TCP port " + std::to_string(servers.direct_port) +
             (spec.code.empty() ? "" : ", and through the match service as game " + spec.code));
    }

    void keep_waiting() {
        if (netband_readable(listener, milliseconds(10))) {
            sockaddr_in from{};
            socklen_t size = sizeof from;
            Socket c = accept(listener, (sockaddr*)&from, &size);
            if (c != kNoSocket) {
                const bool hello = netband_greeted(c);
                close_socket(c);
                if (hello) {
                    if (registered) query("timeout");
                    registered = false;
                    ring({netband_dotted(from), servers.dial_port});
                    return;
                }
            }
        }
        if (spec.code.empty()) return;
        const Clock::time_point now = Clock::now();
        if (now - last_stun >= kStunEvery) {
            last_stun = now;
            std::optional<NetBandAddress> me = stun();
            if (!me && ip12.empty() && !stun_unanswered) {
                stun_unanswered = true;
                note("STUN at " + servers.stun.ip + " did not say where this end is; asking again every 5 s");
            }
            if (me && (netband_ip12(me->ip) != ip12 || me->port != mapped_port)) {
                if (registered) query("timeout");
                registered = false;
                ip12 = netband_ip12(me->ip);
                mapped_port = me->port;
            }
        }
        if (ip12.empty()) return;
        if (!registered) {
            std::string error;
            registered = query("wait", &error).has_value();
            note(registered ? "the match service has this end at " + ip12 + ":" + std::to_string(mapped_port)
                            : "the match service: " + error);
            if (!registered) last_stun = now;   // try again after the next look-up
            return;
        }
        if (now - last_status < kStatusEvery) return;
        last_status = now;
        if (std::optional<std::string> reply = query("status"))
            if (std::optional<NetBandAddress> found = netband_opponent(*reply, "matched")) {
                registered = false;
                ring(*found);
            }
    }

    void make_call() {
        if (spec.ip.empty() && spec.code.empty()) return fail("this end only waits; a call needs the game's code or an address");
        if (!spec.ip.empty()) {
            if (!direct_call()) return;
            begin({spec.ip, servers.wait_port}, true);
            return;
        }
        if (!open_udp(servers.dial_port)) return fail("UDP port " + std::to_string(servers.dial_port) + " is in use");
        std::optional<NetBandAddress> me = stun();
        if (!me) return fail("STUN at " + servers.stun.ip + " did not say where this end is");
        std::string error;
        std::optional<std::string> reply = query_as("match", netband_ip12(me->ip), me->port, &error);
        if (!reply) return fail("the match service: " + error);
        std::optional<NetBandAddress> found = netband_opponent(*reply, "found opponent");
        if (!found) return fail("no game is waiting as game " + spec.code);
        begin(*found, true);
    }

    bool direct_call() {
        std::optional<sockaddr_in> to = netband_resolve({spec.ip, servers.direct_port});
        const bool ok = to && netband_greet(*to);
        if (!ok) fail(spec.ip + " did not take the call on TCP port " + std::to_string(servers.direct_port));
        return ok;
    }

    void ring(const NetBandAddress& from) {
        if (listener != kNoSocket) close_socket(listener);
        listener = kNoSocket;
        note("a call from " + from.ip + ":" + std::to_string(from.port));
        begin(from, false);
        emit({.kind = NetBandEvent::Kind::Ringing});
    }

    void begin(const NetBandAddress& to, bool dialled) {
        std::optional<sockaddr_in> a = netband_resolve(to);
        const int port = dialled ? servers.dial_port : servers.wait_port;
        if (!a) return fail("cannot reach " + to.ip);
        if (!open_udp(port)) return fail("UDP port " + std::to_string(port) + " is in use");
        far_addr = *a;
        sender = {};
        receiver = {};
        held.clear();
        answered = dialled;
        state = State::Up;
        last_heard = last_ping = Clock::now();
        send_to_far(netband_signal_datagram(NetBandSignal::Open));
        if (dialled) {
            note("through to " + to.ip + ":" + std::to_string(to.port));
            emit({.kind = NetBandEvent::Kind::Answered});
        }
    }

    void carry() {
        if (netband_readable(udp, milliseconds(4))) {
            std::vector<uint8_t> d(65536);
            sockaddr_in from{};
            socklen_t size = sizeof from;
            for (int n; (n = (int)recvfrom(udp, (char*)d.data(), (int)d.size(), 0, (sockaddr*)&from, &size)) > 0;
                 size = sizeof from)
                if (from.sin_addr.s_addr == far_addr.sin_addr.s_addr) heard({d.begin(), d.begin() + n});
        }
        std::vector<uint8_t> out;
        {
            std::lock_guard lock(m);
            out.swap(outgoing);
        }
        for (size_t at = 0; at < out.size(); at += kMostPerPayload) {
            const auto end = out.begin() + (ptrdiff_t)std::min(out.size(), at + kMostPerPayload);
            std::vector<uint8_t> d = sender.datagram({out.begin() + (ptrdiff_t)at, end});
            send_to_far(d);   // twice, as the tunnel sends every datagram
            send_to_far(d);
        }
        const Clock::time_point now = Clock::now();
        if (now - last_ping >= kPingEvery) {
            last_ping = now;
            send_to_far(netband_signal_datagram(NetBandSignal::Ping));
        }
        if (now - last_heard > milliseconds(servers.quiet_ms)) {
            stop();
            emit({.kind = NetBandEvent::Kind::Ended, .text = "the other end has gone quiet"});
        }
    }

    void heard(const std::vector<uint8_t>& d) {
        last_heard = Clock::now();
        switch (netband_signal(d)) {
        case NetBandSignal::Ping: send_to_far(netband_signal_datagram(NetBandSignal::Pong)); return;
        case NetBandSignal::Pong:
        case NetBandSignal::Open: return;
        case NetBandSignal::None: break;
        }
        if (stun_message(d)) return;
        std::vector<uint8_t> bytes = receiver.take(d);
        if (bytes.empty()) return;
        if (!answered) held.insert(held.end(), bytes.begin(), bytes.end());
        else emit({.kind = NetBandEvent::Kind::Data, .data = std::move(bytes)});
    }

    void send_to_far(const std::vector<uint8_t>& d) {
        sendto(udp, (const char*)d.data(), (int)d.size(), 0, (const sockaddr*)&far_addr, sizeof far_addr);
    }

    // Where STUN says this end's UDP socket is, asked from that socket.
    std::optional<NetBandAddress> stun() {
        std::optional<sockaddr_in> server = netband_resolve(servers.stun);
        if (!server || udp == kNoSocket) return std::nullopt;
        std::array<uint8_t, 12> id;
        for (uint8_t& b : id) b = (uint8_t)rng();
        return netband_stun(udp, *server, id, stopping);
    }

    std::optional<std::string> query(const std::string& action, std::string* error = nullptr) {
        return query_as(action, ip12, mapped_port, error);
    }

    std::optional<std::string> query_as(const std::string& action, const std::string& ip, int port, std::string* error) {
        std::string why;
        std::optional<std::string> reply = http_get(servers.match + "?" + netband_query(action, spec.code, ip, port), why);
        if (error) *error = why;
        return reply;
    }

    void fail(const std::string& why) {
        stop();
        emit({.kind = NetBandEvent::Kind::Ended, .text = why});
    }

    // Back to idle, closing whatever was open, and taking this end off the match service.
    void stop() {
        if (registered) query("timeout");
        registered = false;
        if (listener != kNoSocket) close_socket(listener);
        if (udp != kNoSocket) close_socket(udp);
        listener = udp = kNoSocket;
        state = State::Idle;
        std::lock_guard lock(m);
        outgoing.clear();
    }
};

NetBand::NetBand(NetBandSpec spec, NetBandServers servers) : impl_(std::make_unique<Impl>()) {
    start_sockets();
    impl_->spec = std::move(spec);
    impl_->servers = std::move(servers);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
}

NetBand::~NetBand() {
    impl_->stopping = true;
    impl_->thread.join();
}

void NetBand::wait() {
    std::lock_guard lock(impl_->m);
    impl_->asks.push_back(Impl::Ask::Wait);
}

void NetBand::call() {
    std::lock_guard lock(impl_->m);
    impl_->asks.push_back(Impl::Ask::Call);
}

void NetBand::answer() {
    std::lock_guard lock(impl_->m);
    impl_->asks.push_back(Impl::Ask::Answer);
}

void NetBand::hang_up() {
    std::lock_guard lock(impl_->m);
    impl_->asks.push_back(Impl::Ask::HangUp);
}

void NetBand::send(const std::vector<uint8_t>& bytes) {
    std::lock_guard lock(impl_->m);
    impl_->outgoing.insert(impl_->outgoing.end(), bytes.begin(), bytes.end());
}

std::vector<NetBandEvent> NetBand::poll() {
    std::lock_guard lock(impl_->m);
    return std::exchange(impl_->events, {});
}

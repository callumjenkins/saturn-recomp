// Two NetBand ends in this process on the loopback address: a direct call, and calls through a stand-in for the
// dreampipe.net match service and a STUN server, each answering as the real ones do (netband_wire.h).
#include "netband.h"
#include "sockets.h"
#include "test.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

NetBandServers loopback(int base) {
    NetBandServers s;
    s.wait_port = base;
    s.dial_port = base + 1;
    s.direct_port = base + 2;
    s.quiet_ms = 1500;
    return s;
}

// The first event of `kind` from `end` within `wait`, with the data events before it gathered into `data`.
std::optional<NetBandEvent> until(NetBand& end, NetBandEvent::Kind kind, std::string* data = nullptr,
                                  std::chrono::milliseconds wait = 8s) {
    for (auto deadline = std::chrono::steady_clock::now() + wait; std::chrono::steady_clock::now() < deadline;) {
        for (NetBandEvent& e : end.poll()) {
            if (e.kind == kind) return e;
            if (e.kind == NetBandEvent::Kind::Data && data) data->append(e.data.begin(), e.data.end());
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::nullopt;
}

std::string data_from(NetBand& end, size_t want) {
    std::string got;
    until(end, NetBandEvent::Kind::Ended, &got, 300ms);
    for (int i = 0; got.size() < want && i < 100; ++i) until(end, NetBandEvent::Kind::Ended, &got, 50ms);
    return got;
}

Socket bound(int type, int port) {
    Socket s = socket(AF_INET, type, 0);
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    bind(s, (sockaddr*)&a, sizeof a);
    if (type == SOCK_STREAM) listen(s, 4);
    return s;
}

bool readable(Socket s) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{0, 20000};
    return select((int)s + 1, &set, nullptr, nullptr, &tv) > 0;
}

// A STUN server and a match service on loopback ports, each on a thread, until it goes.
struct Services {
    Socket stun, http;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::map<std::string, std::string> waiting;   // game ID to the waiter's "ip12", port
    std::map<std::string, std::string> matched;   // a waiter's ip12 to its caller's "ip12", port
    std::thread stun_thread, http_thread;

    explicit Services(int base) : stun(bound(SOCK_DGRAM, base)), http(bound(SOCK_STREAM, base + 1)) {
        stun_thread = std::thread([this] {
            while (!stopping) {
                if (!readable(stun)) continue;
                uint8_t q[64];
                sockaddr_in from{};
                socklen_t size = sizeof from;
                if (recvfrom(stun, (char*)q, sizeof q, 0, (sockaddr*)&from, &size) < 20) continue;
                const uint16_t port = ntohs(from.sin_port) ^ 0x2112;
                const uint32_t ip = ntohl(from.sin_addr.s_addr) ^ 0x2112A442;
                uint8_t r[32] = {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42};
                std::memcpy(r + 8, q + 8, 12);
                const uint8_t attribute[12] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, uint8_t(port >> 8), uint8_t(port),
                                               uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip)};
                std::memcpy(r + 20, attribute, sizeof attribute);
                sendto(stun, (const char*)r, sizeof r, 0, (sockaddr*)&from, size);
            }
        });
        http_thread = std::thread([this] {
            while (!stopping) {
                if (!readable(http)) continue;
                Socket c = accept(http, nullptr, nullptr);
                char request[1024] = {};
                recv(c, request, sizeof request - 1, 0);
                std::string body = answer(request);
                std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                send(c, reply.data(), (int)reply.size(), 0);
                close_socket(c);
            }
        });
    }

    ~Services() {
        stopping = true;
        stun_thread.join();
        http_thread.join();
        close_socket(stun);
        close_socket(http);
    }

    static std::string field(const std::string& request, const std::string& name) {
        size_t at = request.find(name + "=");
        if (at == std::string::npos) return "";
        at += name.size() + 1;
        return request.substr(at, request.find_first_of("& ", at) - at);
    }

    std::string answer(const std::string& request) {
        const std::string action = field(request, "action"), game = field(request, "gameID");
        const std::string me = "\"" + field(request, "client_ip") + "\", \"" + field(request, "port") + "\"";
        std::lock_guard lock(mutex);
        if (action == "wait") waiting[game] = me;
        if (action == "timeout") waiting.erase(game);
        if (action == "status") {
            auto m = matched.find(field(request, "client_ip"));
            if (m != matched.end()) return R"({"status": "matched", "opponent ip_address": [)" + m->second + "]}";
        }
        if (action == "match") {
            auto w = waiting.find(game);
            if (w == waiting.end()) return R"({"status": "no match"})";
            const std::string waiter = w->second;
            matched[waiter.substr(1, 12)] = me;
            waiting.erase(w);
            return R"({"status": "found opponent", "opponent ip_address": [)" + waiter + "]}";
        }
        return R"({"status": "waiting"})";
    }
};

size_t services_waiting(Services& s) {
    std::lock_guard lock(s.mutex);
    return s.waiting.size();
}

NetBandServers through(const Services&, int base, int services) {
    NetBandServers s = loopback(base);
    s.stun = {"127.0.0.1", services};
    s.match = "http://127.0.0.1:" + std::to_string(services + 1) + "/match_service.php";
    return s;
}

}  // namespace

TEST(a_direct_call_rings_the_waiting_end_and_carries_bytes_both_ways) {
    start_sockets();
    const NetBandServers servers = loopback(27110);
    NetBand waiter({.wait = true}, servers), caller({.ip = "127.0.0.1"}, servers);
    waiter.wait();
    CHECK_EQ(until(waiter, NetBandEvent::Kind::Note).has_value(), true);
    caller.call();
    CHECK_EQ(until(caller, NetBandEvent::Kind::Answered).has_value(), true);
    CHECK_EQ(until(waiter, NetBandEvent::Kind::Ringing).has_value(), true);
    caller.send(bytes("before the answer"));
    std::this_thread::sleep_for(100ms);
    waiter.answer();
    CHECK_EQ(data_from(waiter, 17) == "before the answer", true);
    waiter.send(bytes("back"));
    CHECK_EQ(data_from(caller, 4) == "back", true);
}

TEST(a_call_ends_once_the_other_end_goes_quiet) {
    start_sockets();
    const NetBandServers servers = loopback(27120);
    NetBand waiter({.wait = true}, servers), caller({.ip = "127.0.0.1"}, servers);
    waiter.wait();
    until(waiter, NetBandEvent::Kind::Note);
    caller.call();
    CHECK_EQ(until(waiter, NetBandEvent::Kind::Ringing).has_value(), true);
    waiter.answer();
    caller.hang_up();
    std::optional<NetBandEvent> ended = until(waiter, NetBandEvent::Kind::Ended, nullptr, 4s);
    CHECK_EQ(ended && ended->text == "the other end has gone quiet", true);
}

TEST(a_direct_call_nobody_waits_for_ends_with_why) {
    start_sockets();
    NetBand caller({.ip = "127.0.0.1"}, loopback(27130));
    caller.call();
    std::optional<NetBandEvent> ended = until(caller, NetBandEvent::Kind::Ended);
    CHECK_EQ(ended && ended->text == "127.0.0.1 did not take the call on TCP port 27132", true);
}

TEST(the_match_service_puts_a_caller_through_to_the_game_waiting_with_its_code) {
    start_sockets();
    Services services(27140);
    NetBand waiter({.wait = true, .code = "123"}, through(services, 27150, 27140));
    NetBand caller({.code = "123"}, through(services, 27150, 27140));
    waiter.wait();
    std::optional<NetBandEvent> note;
    for (int i = 0; i < 3 && (!note || note->text.find("match service has") == std::string::npos); ++i)
        note = until(waiter, NetBandEvent::Kind::Note);
    CHECK_EQ(note && note->text == "netband: the match service has this end at 127000000001:27150", true);
    caller.call();
    CHECK_EQ(until(caller, NetBandEvent::Kind::Answered).has_value(), true);
    CHECK_EQ(until(waiter, NetBandEvent::Kind::Ringing).has_value(), true);
    waiter.answer();
    caller.send(bytes("matched"));
    CHECK_EQ(data_from(waiter, 7) == "matched", true);
    waiter.send(bytes("back"));
    CHECK_EQ(data_from(caller, 4) == "back", true);
}

TEST(a_call_through_the_match_service_with_nobody_waiting_ends_with_why) {
    start_sockets();
    Services services(27160);
    NetBand caller({.code = "199"}, through(services, 27170, 27160));
    caller.call();
    std::optional<NetBandEvent> ended = until(caller, NetBandEvent::Kind::Ended);
    CHECK_EQ(ended && ended->text == "no game is waiting as game 199", true);
}

TEST(a_ringing_call_gives_no_data_until_it_is_answered) {
    start_sockets();
    const NetBandServers servers = loopback(27180);
    NetBand waiter({.wait = true}, servers), caller({.ip = "127.0.0.1"}, servers);
    waiter.wait();
    until(waiter, NetBandEvent::Kind::Note);
    caller.call();
    CHECK_EQ(until(waiter, NetBandEvent::Kind::Ringing).has_value(), true);
    caller.send(bytes("early"));
    std::string data;
    until(waiter, NetBandEvent::Kind::Ended, &data, 500ms);
    CHECK_EQ(data.empty(), true);
    waiter.answer();
    CHECK_EQ(data_from(waiter, 5) == "early", true);
}

TEST(a_waiting_end_takes_a_second_call_after_the_first_goes_quiet) {
    start_sockets();
    const NetBandServers servers = loopback(27190);
    NetBand waiter({.wait = true}, servers);
    for (const char* text : {"first", "second"}) {
        NetBand caller({.ip = "127.0.0.1"}, servers);
        waiter.wait();
        until(waiter, NetBandEvent::Kind::Note);
        caller.call();
        CHECK_EQ(until(waiter, NetBandEvent::Kind::Ringing).has_value(), true);
        waiter.answer();
        caller.send(bytes(text));
        CHECK_EQ(data_from(waiter, std::strlen(text)) == text, true);
        caller.hang_up();
        CHECK_EQ(until(waiter, NetBandEvent::Kind::Ended, nullptr, 4s).has_value(), true);
    }
}

TEST(a_send_larger_than_a_datagram_arrives_whole) {
    start_sockets();
    const NetBandServers servers = loopback(27200);
    NetBand waiter({.wait = true}, servers), caller({.ip = "127.0.0.1"}, servers);
    waiter.wait();
    until(waiter, NetBandEvent::Kind::Note);
    caller.call();
    until(waiter, NetBandEvent::Kind::Ringing);
    waiter.answer();
    std::string big(3000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char('a' + i % 26);
    caller.send(bytes(big));
    caller.send(bytes("after"));
    CHECK_EQ(data_from(waiter, big.size() + 5) == big + "after", true);
}

TEST(a_wait_only_end_that_dials_ends_with_why) {
    start_sockets();
    NetBand end({.wait = true}, loopback(27210));
    end.call();
    std::optional<NetBandEvent> ended = until(end, NetBandEvent::Kind::Ended);
    CHECK_EQ(ended && ended->text == "this end only waits; a call needs the game's code or an address", true);
}

TEST(a_wait_hung_up_takes_the_game_off_the_match_service) {
    start_sockets();
    Services services(27220);
    NetBand waiter({.wait = true, .code = "123"}, through(services, 27230, 27220));
    waiter.wait();
    for (int i = 0; i < 3 && services_waiting(services) == 0; ++i) until(waiter, NetBandEvent::Kind::Note);
    CHECK_EQ(services_waiting(services), 1);
    waiter.hang_up();
    for (int i = 0; i < 100 && services_waiting(services) != 0; ++i) std::this_thread::sleep_for(20ms);
    CHECK_EQ(services_waiting(services), 0);
}

// The far end as the tunnel's own client is, on raw sockets: its words on TCP, then its datagrams.
TEST(the_wire_is_the_tunnels_hello_open_pong_and_every_datagram_twice) {
    start_sockets();
    const NetBandServers servers = loopback(27240);
    NetBand waiter({.wait = true}, servers);
    waiter.wait();
    until(waiter, NetBandEvent::Kind::Note);
    Socket udp = bound(SOCK_DGRAM, servers.dial_port);
    Socket tcp = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons((uint16_t)servers.direct_port);
    CHECK_EQ(connect(tcp, (sockaddr*)&to, sizeof to), 0);
    send(tcp, "readyip", 7, 0);
    bool replied = false;
    for (int i = 0; i < 100 && !replied; ++i) replied = readable(tcp);
    CHECK_EQ(replied, true);
    char word[8] = {};
    recv(tcp, word, sizeof word - 1, 0);
    CHECK_EQ(std::string(word) == "g2gip", true);
    close_socket(tcp);
    CHECK_EQ(until(waiter, NetBandEvent::Kind::Ringing).has_value(), true);

    // Every datagram to the far end, signals included, in the order it came.
    auto next = [&](const char* skip = nullptr) {
        for (int i = 0; i < 100; ++i) {
            if (!readable(udp)) continue;
            char d[2048];
            int n = (int)recv(udp, d, sizeof d, 0);
            std::string got(d, n > 0 ? n : 0);
            if (!skip || got != skip) return got;
        }
        return std::string();
    };
    CHECK_EQ(next() == "OPEN_SHIRO", true);
    to.sin_port = htons((uint16_t)servers.wait_port);
    sendto(udp, "PING_SHIRO", 10, 0, (sockaddr*)&to, sizeof to);
    CHECK_EQ(next("PING_SHIRO") == "PONG_SHIRO", true);
    waiter.answer();
    waiter.send(bytes("x"));
    CHECK_EQ(next("PING_SHIRO") == "x<dataSplit>0", true);
    CHECK_EQ(next("PING_SHIRO") == "x<dataSplit>0", true);
    close_socket(udp);
}

TEST(a_waiting_end_whose_stun_does_not_answer_says_so) {
    start_sockets();
    NetBandServers servers = loopback(27250);
    servers.stun = {"127.0.0.1", 27259};   // nobody there
    NetBand waiter({.wait = true, .code = "123"}, servers);
    waiter.wait();
    std::optional<NetBandEvent> note;
    for (int i = 0; i < 3 && (!note || note->text.find("STUN") == std::string::npos); ++i)
        note = until(waiter, NetBandEvent::Kind::Note);
    CHECK_EQ(note && note->text == "netband: STUN at 127.0.0.1 did not say where this end is; asking again every 5 s", true);
}

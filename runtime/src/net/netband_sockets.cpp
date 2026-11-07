// saturn-recomp runtime — the NetBand tunnel's sockets (netband_sockets.h).
#include "netband_sockets.h"
#include <cstring>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

const milliseconds kHandshake(2000);   // a direct call's TCP connection, and each of its words

bool hear_word(Socket s, const char* want) {
    char got[16] = {};
    if (!netband_readable(s, kHandshake)) return false;
    int n = (int)recv(s, got, sizeof got - 1, 0);
    return n > 0 && std::strcmp(got, want) == 0;
}

sockaddr_in any_address(int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    return a;
}

}  // namespace

std::optional<sockaddr_in> netband_resolve(const NetBandAddress& a) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(a.ip.c_str(), std::to_string(a.port).c_str(), &hints, &res) != 0 || !res) return std::nullopt;
    sockaddr_in out;
    std::memcpy(&out, res->ai_addr, sizeof out);
    freeaddrinfo(res);
    return out;
}

std::string netband_dotted(const sockaddr_in& a) {
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, text, sizeof text);
    return text;
}

bool netband_readable(Socket s, milliseconds wait) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{(long)(wait.count() / 1000), (long)(wait.count() % 1000 * 1000)};
    return select((int)s + 1, &set, nullptr, nullptr, &tv) > 0;
}

Socket netband_udp(int port) {
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return kNoSocket;
    int tos = 184;   // the tunnel's: expedited forwarding
    setsockopt(s, IPPROTO_IP, IP_TOS, (const char*)&tos, sizeof tos);
    const sockaddr_in a = any_address(port);
    if (bind(s, (const sockaddr*)&a, sizeof a) != 0) {
        close_socket(s);
        return kNoSocket;
    }
    nonblocking(s);
    return s;
}

Socket netband_listener(int port) {
    Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) return kNoSocket;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    const sockaddr_in a = any_address(port);
    if (bind(s, (const sockaddr*)&a, sizeof a) != 0 || listen(s, 1) != 0) {
        close_socket(s);
        return kNoSocket;
    }
    nonblocking(s);
    return s;
}

bool netband_greeted(Socket c) {
    if (!hear_word(c, "readyip")) return false;
    ::send(c, "g2gip", 5, 0);
    return true;
}

bool netband_greet(const sockaddr_in& to) {
    Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) return false;
    nonblocking(s);
    connect(s, (const sockaddr*)&to, sizeof to);
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{(long)(kHandshake.count() / 1000), 0};
    int error = 0;
    socklen_t size = sizeof error;
    const bool ok = select((int)s + 1, nullptr, &set, nullptr, &tv) > 0 &&
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&error, &size) == 0 && error == 0 &&
                    ::send(s, "readyip", 7, 0) == 7 && hear_word(s, "g2gip");
    close_socket(s);
    return ok;
}

std::optional<NetBandAddress> netband_stun(Socket udp, const sockaddr_in& server, const std::array<uint8_t, 12>& id,
                                           const std::atomic<bool>& stopping) {
    const std::vector<uint8_t> request = stun_request(id);
    for (int attempt = 0; attempt < 3 && !stopping; ++attempt) {
        sendto(udp, (const char*)request.data(), (int)request.size(), 0, (const sockaddr*)&server, sizeof server);
        const Clock::time_point until = Clock::now() + milliseconds(700);
        while (Clock::now() < until && netband_readable(udp, milliseconds(50))) {
            std::vector<uint8_t> d(1024);
            int n = (int)recv(udp, (char*)d.data(), (int)d.size(), 0);
            if (n <= 0) continue;
            d.resize(n);
            if (std::optional<NetBandAddress> me = stun_mapped(d, id)) return me;
        }
    }
    return std::nullopt;
}

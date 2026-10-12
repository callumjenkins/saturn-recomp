// saturn-recomp runtime — a modem's phone line as a TCP connection (modem.h): a dial connects to the one
// address the spec names, whatever the number, and a connection to the listening port rings.
#include "modem.h"
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
static const Socket kNone = INVALID_SOCKET;
static void close_socket(Socket s) { closesocket(s); }
static void nonblocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
static bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
static bool start_sockets() { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
static const Socket kNone = -1;
static void close_socket(Socket s) { close(s); }
static void nonblocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
static bool start_sockets() { return true; }
#endif

namespace {

const uint8_t kPickedUp = 0x01;              // what the answering end sends first, before any data

void no_delay(Socket s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
}

struct TcpLine : ModemLine {
    std::string host, port;                      // where a dial goes; empty: nowhere
    bool up = false;                             // the call is made (or waited for) at power-on
    Socket listener = kNone, waiting = kNone, call = kNone;
    bool picked_up = false;                      // the far end of the call dialled has answered

    ~TcpLine() override {
        hang_up();
        if (waiting != kNone) close_socket(waiting);
        if (listener != kNone) close_socket(listener);
    }

    bool listen_on(const std::string& p) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        if (getaddrinfo(nullptr, p.c_str(), &hints, &res) != 0) return false;
        listener = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        int on = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
        bool ok = listener != kNone && bind(listener, res->ai_addr, (int)res->ai_addrlen) == 0 && ::listen(listener, 1) == 0;
        freeaddrinfo(res);
        if (ok) nonblocking(listener);
        return ok;
    }

    bool dial(const std::string&) override {
        if (host.empty() || call != kNone) return false;
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return false;
        for (addrinfo* a = res; a && call == kNone; a = a->ai_next) {
            Socket s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == kNone) continue;
            if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) call = s;
            else close_socket(s);
        }
        freeaddrinfo(res);
        if (call == kNone) return false;
        nonblocking(call);
        no_delay(call);
        picked_up = false;
        return true;
    }

    bool answered() override {
        if (call == kNone) return false;
        uint8_t b;
        if (!picked_up && recv(call, (char*)&b, 1, 0) == 1) picked_up = b == kPickedUp;
        return picked_up;
    }

    bool ringing() override {
        if (call != kNone || listener == kNone) return false;
        if (waiting == kNone) waiting = accept(listener, nullptr, nullptr);
        return waiting != kNone;
    }

    bool answer() override {
        if (waiting == kNone) return false;
        call = waiting;
        waiting = kNone;
        nonblocking(call);
        no_delay(call);
        ::send(call, (const char*)&kPickedUp, 1, 0);
        return true;
    }

    void hang_up() override {
        if (call != kNone) close_socket(call);
        call = kNone;
    }

    bool connected() override {
        if (call == kNone) return false;
        char b;
        int n = (int)recv(call, &b, 1, MSG_PEEK);
        if (n == 0 || (n < 0 && !would_block())) {
            hang_up();
            return false;
        }
        return true;
    }

    void send(uint8_t b) override {
        if (call != kNone) ::send(call, (const char*)&b, 1, 0);
    }

    bool receive(uint8_t& b) override {
        return call != kNone && recv(call, (char*)&b, 1, 0) == 1;
    }

    bool leased() override { return up; }
};

}  // namespace

std::unique_ptr<ModemLine> modem_tcp_line(const std::string& spec, std::string& error) {
    if (!start_sockets()) { error = "sockets are not available"; return nullptr; }
    auto line = std::make_unique<TcpLine>();
    size_t at = 0;
    while (at <= spec.size()) {
        size_t end = spec.find(',', at);
        std::string part = spec.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? spec.size() + 1 : end + 1;
        if (part.rfind("dial=", 0) == 0) {
            size_t colon = part.rfind(':');
            if (colon == std::string::npos || colon < 5) { error = "dial= takes HOST:PORT"; return nullptr; }
            line->host = part.substr(5, colon - 5);
            line->port = part.substr(colon + 1);
        } else if (part.rfind("listen=", 0) == 0) {
            if (!line->listen_on(part.substr(7))) { error = "cannot listen on port " + part.substr(7); return nullptr; }
        } else if (part == "up") {
            line->up = true;
        } else if (!part.empty()) {
            error = "unknown modem setting \"" + part + "\" (dial=HOST:PORT, listen=PORT, up)";
            return nullptr;
        }
    }
    if (line->up && line->host.empty() && line->listener == kNone) { error = "up needs dial= or listen="; return nullptr; }
    return line;
}

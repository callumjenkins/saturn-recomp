// http_get over plain HTTP, against a server on a loopback port that sends one canned reply to each connection.
#include "http_get.h"
#include "sockets.h"
#include "test.h"
#include <atomic>
#include <string>
#include <thread>

namespace {

struct Server {
    Socket s;
    std::atomic<bool> stopping{false};
    std::thread thread;

    Server(int port, std::string reply) : s(socket(AF_INET, SOCK_STREAM, 0)) {
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        bind(s, (sockaddr*)&a, sizeof a);
        listen(s, 4);
        thread = std::thread([this, reply = std::move(reply)] {
            while (!stopping) {
                fd_set set;
                FD_ZERO(&set);
                FD_SET(s, &set);
                timeval tv{0, 20000};
                if (select((int)s + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
                Socket c = accept(s, nullptr, nullptr);
                char request[1024];
                recv(c, request, sizeof request, 0);
                send(c, reply.data(), (int)reply.size(), 0);
                close_socket(c);
            }
        });
    }
    ~Server() {
        stopping = true;
        thread.join();
        close_socket(s);
    }
};

std::string url(int port) { return "http://127.0.0.1:" + std::to_string(port) + "/match_service.php?action=status"; }

}  // namespace

TEST(http_get_reads_a_plain_or_chunked_body) {
    start_sockets();
    std::string error;
    {
        Server server(27300, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        CHECK_EQ(http_get(url(27300), error).value_or("") == "ok", true);
    }
    Server server(27301, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    CHECK_EQ(http_get(url(27301), error).value_or("") == "hello world", true);
}

TEST(http_get_says_why_it_has_no_body) {
    start_sockets();
    struct Case {
        int port;
        const char* reply;
        const char* why;
    };
    for (const Case& c : {Case{27310, "HTTP/1.1 404 Not Found\r\n\r\nno", "127.0.0.1 answered with status 404"},
                          Case{27311, "SSH-2.0-OpenSSH\r\n", "127.0.0.1 sent something other than HTTP"},
                          Case{27312, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n9\r\nhalf", "127.0.0.1's reply was cut short"}}) {
        Server server(c.port, c.reply);
        std::string error;
        CHECK_EQ(http_get(url(c.port), error).has_value(), false);
        CHECK_EQ(error == c.why, true);
    }
    std::string error;
    CHECK_EQ(http_get("ftp://127.0.0.1/", error).has_value(), false);
    CHECK_EQ(error == "ftp://127.0.0.1/ is not an http:// or https:// URL", true);
    CHECK_EQ(http_get(url(27319), error, 2000).has_value(), false);
    CHECK_EQ(error == "cannot connect to 127.0.0.1:27319", true);
}

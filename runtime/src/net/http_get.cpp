// saturn-recomp runtime — an HTTP GET over Mbed TLS's sockets (http_get.h).
#include "http_get.h"
#include "ca_bundle.h"
#include "mbedtls_threads.h"
#include "sockets.h"
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {

struct Url {
    bool tls = false;
    std::string host, port, target;
};

std::optional<Url> parse(const std::string& url) {
    Url u;
    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        u.tls = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        rest = url.substr(7);
    } else {
        return std::nullopt;
    }
    size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    u.target = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = authority.rfind(':');
    u.host = authority.substr(0, colon);
    u.port = colon == std::string::npos ? (u.tls ? "443" : "80") : authority.substr(colon + 1);
    if (u.host.empty()) return std::nullopt;
    return u;
}

// A TCP connection to `host`:`port` within `timeout_ms`, which mbedtls_net_connect cannot bound.
Socket connect_within(const std::string& host, const std::string& port, int timeout_ms) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0) return kNoSocket;
    Socket s = kNoSocket;
    for (addrinfo* a = found; a && s == kNoSocket; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSocket) continue;
        nonblocking(s);
        connect(s, a->ai_addr, (int)a->ai_addrlen);
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval tv{timeout_ms / 1000, timeout_ms % 1000 * 1000};
        int failed = 0;
        socklen_t size = sizeof failed;
        if (select((int)s + 1, nullptr, &set, nullptr, &tv) > 0 &&
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&failed, &size) == 0 && failed == 0) {
            blocking(s);
        } else {
            close_socket(s);
            s = kNoSocket;
        }
    }
    freeaddrinfo(found);
    return s;
}

// A chunked body's data, or nothing if it is cut short.
std::optional<std::string> dechunk(const std::string& body) {
    std::string out;
    size_t at = 0;
    for (;;) {
        size_t eol = body.find("\r\n", at);
        if (eol == std::string::npos) return std::nullopt;
        size_t size = std::strtoul(body.substr(at, eol - at).c_str(), nullptr, 16);
        if (size == 0) return out;
        if (eol + 2 + size > body.size()) return std::nullopt;
        out += body.substr(eol + 2, size);
        at = eol + 2 + size + 2;
    }
}

// The connection, over TLS or not, and everything Mbed TLS needs for it.
struct Connection {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool tls = false;

    Connection() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&ca);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Connection() {
        if (tls) mbedtls_ssl_close_notify(&ssl);
        mbedtls_net_free(&net);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&ca);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool secure(const std::string& host, int timeout_ms, std::string& error) {
        const std::string& pem = ca_bundle_pem();
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0) != 0 ||
            mbedtls_x509_crt_parse(&ca, reinterpret_cast<const unsigned char*>(pem.c_str()), pem.size() + 1) < 0 ||
            mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            error = "TLS could not be set up";
            return false;
        }
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_read_timeout(&conf, timeout_ms);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0 || mbedtls_ssl_set_hostname(&ssl, host.c_str()) != 0) {
            error = "TLS could not be set up";
            return false;
        }
        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
        tls = true;
        for (int r; (r = mbedtls_ssl_handshake(&ssl)) != 0;) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            error = mbedtls_ssl_get_verify_result(&ssl) != 0 ? host + "'s certificate is not trusted"
                                                              : "the TLS handshake with " + host + " failed";
            return false;
        }
        return true;
    }

    bool write(const std::string& data) {
        for (size_t at = 0; at < data.size();) {
            const auto* p = reinterpret_cast<const unsigned char*>(data.data() + at);
            int n = tls ? mbedtls_ssl_write(&ssl, p, data.size() - at) : mbedtls_net_send(&net, p, data.size() - at);
            if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (n <= 0) return false;
            at += n;
        }
        return true;
    }

    // Everything until the server closes the connection; false if it stops answering first.
    bool read_all(std::string& out, int timeout_ms) {
        unsigned char buf[4096];
        for (;;) {
            int n = tls ? mbedtls_ssl_read(&ssl, buf, sizeof buf) : mbedtls_net_recv_timeout(&net, buf, sizeof buf, timeout_ms);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return true;
            if (n < 0) return n != MBEDTLS_ERR_SSL_TIMEOUT && !out.empty();
            out.append(reinterpret_cast<const char*>(buf), n);
        }
    }
};

}  // namespace

std::optional<std::string> http_get(const std::string& url, std::string& error, int timeout_ms) {
    std::optional<Url> u = parse(url);
    if (!u) {
        error = url + " is not an http:// or https:// URL";
        return std::nullopt;
    }
    if (u->tls) {
        mbedtls_threads_start();
        psa_crypto_init();
    }
    start_sockets();
    Connection c;
    const Socket s = connect_within(u->host, u->port, timeout_ms);
    if (s == kNoSocket) {
        error = "cannot connect to " + u->host + ":" + u->port;
        return std::nullopt;
    }
    c.net.fd = (int)s;   // Mbed TLS keeps a Winsock SOCKET in an int too
    if (u->tls && !c.secure(u->host, timeout_ms, error)) return std::nullopt;
    std::string reply;
    if (!c.write("GET " + u->target + " HTTP/1.1\r\nHost: " + u->host + "\r\nUser-Agent: saturn-recomp\r\n" +
                 "Accept: */*\r\nConnection: close\r\n\r\n") ||
        !c.read_all(reply, timeout_ms)) {
        error = u->host + " did not answer";
        return std::nullopt;
    }
    size_t end = reply.find("\r\n\r\n");
    int status = 0;
    if (end == std::string::npos || std::sscanf(reply.c_str(), "HTTP/%*s %d", &status) != 1) {
        error = u->host + " sent something other than HTTP";
        return std::nullopt;
    }
    if (status < 200 || status > 299) {
        error = u->host + " answered with status " + std::to_string(status);
        return std::nullopt;
    }
    std::string head = reply.substr(0, end);
    for (char& ch : head) ch = (char)std::tolower((unsigned char)ch);
    std::string body = reply.substr(end + 4);
    if (head.find("transfer-encoding: chunked") == std::string::npos) return body;
    std::optional<std::string> data = dechunk(body);
    if (!data) error = u->host + "'s reply was cut short";
    return data;
}

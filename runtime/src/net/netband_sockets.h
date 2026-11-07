// saturn-recomp runtime — the NetBand tunnel's sockets (netband.h): its UDP and TCP ports, the TCP words a direct
// call opens with, and a STUN look-up.
#pragma once
#include "netband_wire.h"
#include "sockets.h"
#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>

std::optional<sockaddr_in> netband_resolve(const NetBandAddress& a);
std::string netband_dotted(const sockaddr_in& a);
bool netband_readable(Socket s, std::chrono::milliseconds wait);

// Non-blocking, or kNoSocket when the port is taken.
Socket netband_udp(int port);
Socket netband_listener(int port);

// The waiting end's half of a direct call: "readyip" heard on `c`, and "g2gip" said back.
bool netband_greeted(Socket c);
// The calling end's half: whether `to` took the connection and answered "readyip" with "g2gip".
bool netband_greet(const sockaddr_in& to);

// Where the STUN server at `server` sees `udp`, asked with `id`; empty after three tries, or once `stopping`.
std::optional<NetBandAddress> netband_stun(Socket udp, const sockaddr_in& server, const std::array<uint8_t, 12>& id,
                                           const std::atomic<bool>& stopping);

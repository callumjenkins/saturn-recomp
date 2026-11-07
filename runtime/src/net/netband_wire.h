// saturn-recomp runtime — what SegaRPGFan's NetBand tunnel puts on the wire, so a recompiled game's modem can call a
// real Saturn's NetLink through it (netband.h): the UDP datagrams of a call, STUN's binding request, and the
// dreampipe.net match service's queries and replies.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

// A call's datagrams are the latest payloads, newest first, each followed by "<dataSplit>" and its sequence number
// in decimal, joined by "<packetSplit>". A payload is whatever the modem sent since the last one.
class NetBandSender {
public:
    std::vector<uint8_t> datagram(const std::vector<uint8_t>& payload);   // the next one, holding `payload`

private:
    std::deque<std::vector<uint8_t>> recent_;   // framed, newest first
    uint64_t sequence_ = 0;
};

// Takes a call's datagrams as the tunnel does: the payload numbered next, then those after it in the same datagram,
// or, with the next one lost, skips ahead to the newest. A datagram whose payloads are all old is dropped.
class NetBandReceiver {
public:
    std::vector<uint8_t> take(const std::vector<uint8_t>& datagram);   // the bytes it delivers, in order

private:
    uint64_t next_ = 0;
};

// Datagrams of a call that carry no payload.
enum class NetBandSignal { None, Ping, Pong, Open };
NetBandSignal netband_signal(const std::vector<uint8_t>& datagram);
std::vector<uint8_t> netband_signal_datagram(NetBandSignal s);

struct NetBandAddress {
    std::string ip;   // dotted
    int port = 0;
    bool operator==(const NetBandAddress&) const = default;
};

// The match service writes an IPv4 address as 12 digits, each part padded to 3: "192168001002".
std::string netband_ip12(const std::string& dotted);
std::optional<std::string> netband_dotted(const std::string& ip12);

// A STUN binding request with transaction `id`, and the address a reply to it maps this end to.
std::vector<uint8_t> stun_request(const std::array<uint8_t, 12>& id);
std::optional<NetBandAddress> stun_mapped(const std::vector<uint8_t>& reply, const std::array<uint8_t, 12>& id);
bool stun_message(const std::vector<uint8_t>& datagram);   // a STUN reply rather than a call's datagram

// The match service's query string for `action` (wait, status, match, or timeout) by the game whose code is `code`,
// the tunnel's game ID being its last two digits. `port` is left out of status and timeout, as the tunnel does.
std::string netband_query(const std::string& action, const std::string& code, const std::string& ip12, int port);
// The other end in a reply whose "status" is `matched`, "matched" to a waiter and "found opponent" to a caller.
std::optional<NetBandAddress> netband_opponent(const std::string& reply, const std::string& matched);

// What --modem netband=SPEC asks for: wait[:CODE] waits for a call, direct and, with the game's code, through the
// match service; dial:CODE finds a waiting game through the match service, and dial:IP calls that address directly.
struct NetBandSpec {
    bool wait = false;
    std::string code;   // 3 digits, as players dial it
    std::string ip;     // dotted, for a direct call
};
std::optional<NetBandSpec> netband_spec(const std::string& text, std::string& error);

// saturn-recomp runtime — a NetLink call through SegaRPGFan's NetBand tunnel, as a DreamPi makes one for a real
// Saturn: found through the dreampipe.net match service, or dialled straight to an address, then the modem's bytes
// over UDP (netband_wire.h). No hang-up crosses the wire, so a call ends when the other end goes quiet.
#pragma once
#include "netband_wire.h"
#include <memory>
#include <string>
#include <vector>

// Where a call's services are, and its ports, which are the tunnel's: a test points them at its own.
struct NetBandServers {
    std::string match = "https://saturn.dreampipe.net/match_service.php";
    NetBandAddress stun{"stun.l.google.com", 19302};
    int wait_port = 20001;     // UDP, the waiting end's
    int dial_port = 20002;     // UDP, the calling end's
    int direct_port = 65432;   // TCP, where a direct call announces itself to the waiting end
    int quiet_ms = 20000;      // the other end silent this long has gone
};

struct NetBandEvent {
    enum class Kind {
        Ringing,    // a call has come to the waiting end
        Answered,   // the call this end made has reached the other
        Ended,      // the call, or the attempt at one, is over, `text` saying how
        Data,       // `data` came down the call, once it is answered
        Note,       // `text` is worth the log
    };
    Kind kind;
    std::vector<uint8_t> data;
    std::string text;
};

// The tunnel's work runs on a thread of its own; the rest is for one other thread, the emulation's.
class NetBand {
public:
    explicit NetBand(NetBandSpec spec, NetBandServers servers = {});
    ~NetBand();
    NetBand(const NetBand&) = delete;
    NetBand& operator=(const NetBand&) = delete;

    void wait();     // waits for a call, when the spec says to wait and no call is going on
    void call();     // dials the spec's address, or finds a waiting game through the match service
    void answer();   // takes the call that is ringing: its data goes to poll from now on
    void hang_up();  // ends the call, or the wait or dial still going on
    void send(const std::vector<uint8_t>& bytes);
    std::vector<NetBandEvent> poll();   // what has happened since the last poll, oldest first

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

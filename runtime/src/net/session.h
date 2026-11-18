// saturn-recomp runtime — a session: the other peers in one room of the playtest Worker (playtest/worker/src/rooms.ts),
// each reached by a WebRTC connection of its own with three data channels.
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A peer's three channels. Pads are unordered and never sent again, since the next frame's pads supersede them;
// the line (a modem's bytes) and control arrive whole and in order.
enum class Channel : uint8_t { Pads, Line, Control };

// What a room requires every peer in it to share.
struct SessionTerms {
    std::string product, game, runtime, mode;
};

// What a peer tells the room as it joins: its tester, by token or by id, its terms, and how many pads it brings.
struct SessionHello {
    std::string token, tester;
    SessionTerms terms;
    int pads = 1;
};

struct SessionEvent {
    enum class Kind {
        Welcome,     // in the room, whose code is `text`, as `peer`
        Joined,      // `peer` is in the room, as `name` with `pads` pads; every peer already there comes first
        Open,        // all of `peer`'s channels are open, `text` naming the kinds of candidate chosen, "host to srflx"
        Message,     // `data` came from `peer` on `channel`
        Left,        // `peer` has gone, `text` saying how
        Refused,     // the room turned this peer away, `text` saying why
        RoomClosed,  // the room's socket closed or never opened, `text` saying why; open connections go on
    };
    Kind kind;
    std::string peer, name, text;
    int pads = 0;
    Channel channel = Channel::Control;
    std::vector<uint8_t> data;
};

// The session's work runs on a thread of its own; poll and send are for one other thread, the emulation's.
class Session {
public:
    // Joins the room at `url`, ws:// or wss:// and ending in /api/rooms/new or /api/rooms/CODE. Unpolled for
    // `away_after`, as a game paused in the background or behind its menu is, the session reports each peer Left
    // and leaves the room.
    Session(const std::string& url, const SessionHello& hello,
            std::chrono::milliseconds away_after = std::chrono::seconds(10));
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    std::vector<SessionEvent> poll();   // what has happened since the last poll, oldest first
    // To `peer`, or to every peer when it is empty. A peer whose channel is not open yet misses it.
    void send(const std::string& peer, Channel channel, std::vector<uint8_t> data);
    // For tests: drops the room's socket and every connection at once, as a network that goes away does.
    void sever();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

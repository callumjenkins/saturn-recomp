// Sessions joined to a room in this process: a stub that speaks the playtest Worker's room messages (rooms.ts) without
// its checks, so two sessions meet, open their channels and exchange each, see each other go, and come back.
#include "session.h"
#include "session_ice.h"
#include "test.h"
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

using json = nlohmann::json;

namespace {

// One room, code ABCD. A hello whose game is "refuse-me" is refused. A peer that goes without a bye can come back,
// for as long as the room lasts.
struct StubRoom {
    rtc::WebSocketServer server;
    std::mutex mutex;
    std::vector<std::shared_ptr<rtc::WebSocket>> clients;   // every socket, kept open until the room goes
    std::map<std::string, std::shared_ptr<rtc::WebSocket>> peers;
    std::map<std::string, bool> said_bye, away;
    int next = 0;

    StubRoom() : server(config()) {
        server.onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
            {
                std::scoped_lock lock(mutex);
                clients.push_back(ws);
            }
            std::weak_ptr<rtc::WebSocket> weak = ws;
            auto id = std::make_shared<std::string>();
            ws->onMessage([this, weak, id](rtc::message_variant m) {
                auto* text = std::get_if<std::string>(&m);
                auto ws = weak.lock();
                if (text && ws) heard(ws, *id, json::parse(*text));
            });
            ws->onClosed([this, id] { gone(*id); });
        });
    }

    static rtc::WebSocketServerConfiguration config() {
        rtc::WebSocketServerConfiguration c;
        c.port = 0;
        c.bindAddress = "127.0.0.1";
        return c;
    }

    ~StubRoom() {
        server.stop();
        std::scoped_lock lock(mutex);
        for (auto& ws : clients) ws->resetCallbacks();
    }

    std::string url(const std::string& path) { return "ws://127.0.0.1:" + std::to_string(server.port()) + path; }

    void heard(const std::shared_ptr<rtc::WebSocket>& ws, std::string& id, const json& m) {
        std::scoped_lock lock(mutex);
        if (m["type"] == "hello" && m["terms"]["game"] == "refuse-me") {
            ws->send(json{{"type", "refused"}, {"reason", "the game differs"}}.dump());
            return;
        }
        if (m["type"] == "bye") {
            said_bye[id] = true;
            return;
        }
        if (m["type"] == "hello" && m.contains("resume") && away.erase(m["resume"])) {
            id = m["resume"];
            welcome(ws, id, m["pads"]);
        } else if (m["type"] == "hello") {
            id = "p" + std::to_string(next++);
            welcome(ws, id, m["pads"]);
        } else if (m["type"] == "signal" && peers.count(m["to"])) {
            peers[m["to"]]->send(json{{"type", "signal"}, {"from", id}, {"data", m["data"]}}.dump());
        }
    }

    void welcome(const std::shared_ptr<rtc::WebSocket>& ws, const std::string& id, const json& pads) {
        json others = json::array();
        for (auto& [other, _] : peers) others.push_back({{"id", other}, {"name", other}, {"pads", 2}});
        ws->send(json{{"type", "welcome"}, {"code", "ABCD"}, {"id", id}, {"peers", others}, {"iceServers", json::array()}}.dump());
        for (auto& [_, other] : peers) other->send(json{{"type", "joined"}, {"peer", {{"id", id}, {"name", id}, {"pads", pads}}}}.dump());
        peers[id] = ws;
    }

    void gone(const std::string& id) {
        std::scoped_lock lock(mutex);
        if (!peers.erase(id)) return;
        if (!said_bye[id]) {
            away[id] = true;
            return;
        }
        for (auto& [_, other] : peers) other->send(json{{"type", "left"}, {"id", id}}.dump());
    }
};

SessionHello hello(const std::string& game = "c73bc11") {
    return {.tester = "t1", .terms = {.product = "MK-81070_V1.003", .game = game, .runtime = "f20de94", .mode = "test"}, .pads = 2};
}

// The session's events until one matches, for up to 15 seconds; every event polled goes into `seen`.
bool wait_for(Session& s, std::vector<SessionEvent>& seen, const std::function<bool(const SessionEvent&)>& match) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < until) {
        for (auto& e : s.poll()) {
            seen.push_back(e);
            if (match(e)) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

auto kind(SessionEvent::Kind k) {
    return [k](const SessionEvent& e) { return e.kind == k; };
}

}  // namespace

TEST(session_peers_meet_and_exchange_every_channel) {
    StubRoom room;
    Session a(room.url("/api/rooms/new"), hello());
    std::vector<SessionEvent> as, bs;
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Welcome)), true);
    CHECK_EQ(as.back().text == "ABCD", true);

    Session b(room.url("/api/rooms/ABCD"), hello());
    CHECK_EQ(wait_for(b, bs, kind(SessionEvent::Kind::Open)), true);
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Open)), true);
    CHECK_EQ(as.back().peer == "p1", true);
    CHECK_EQ(bs.back().peer == "p0", true);
    // b heard of a in its welcome, and a heard b join, with its pads
    auto joined = std::find_if(as.begin(), as.end(), kind(SessionEvent::Kind::Joined));
    CHECK_EQ(joined != as.end() && joined->peer == "p1" && joined->pads == 2, true);

    for (Channel c : {Channel::Pads, Channel::Line, Channel::Control}) {
        a.send("p1", c, {(uint8_t)c, 0xAA, 0x55});
        CHECK_EQ(wait_for(b, bs, kind(SessionEvent::Kind::Message)), true);
        CHECK_EQ(bs.back().channel == c && bs.back().peer == "p0", true);
        CHECK_EQ(bs.back().data == std::vector<uint8_t>({(uint8_t)c, 0xAA, 0x55}), true);
    }
    b.send("", Channel::Line, {1, 2, 3});
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Message)), true);
    CHECK_EQ(as.back().data == std::vector<uint8_t>({1, 2, 3}), true);
}

TEST(session_hears_a_peer_leave) {
    StubRoom room;
    Session a(room.url("/api/rooms/new"), hello());
    std::vector<SessionEvent> as, bs;
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Welcome)), true);
    {
        Session b(room.url("/api/rooms/ABCD"), hello());
        CHECK_EQ(wait_for(b, bs, kind(SessionEvent::Kind::Open)), true);
        CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Open)), true);
    }
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Left)), true);
    CHECK_EQ(as.back().peer == "p1", true);
}

// The network under b goes: it joins the room again as itself, and both connect again with nobody reported gone.
TEST(session_comes_back_to_the_room_after_its_network_goes) {
    StubRoom room;
    Session a(room.url("/api/rooms/new"), hello());
    std::vector<SessionEvent> as, bs;
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Welcome)), true);
    Session b(room.url("/api/rooms/ABCD"), hello());
    CHECK_EQ(wait_for(b, bs, kind(SessionEvent::Kind::Open)), true);
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Open)), true);

    b.sever();
    CHECK_EQ(wait_for(b, bs, kind(SessionEvent::Kind::Open)), true);
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Open)), true);
    CHECK_EQ(as.back().peer == "p1", true);
    b.send("p0", Channel::Control, {7});
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Message)), true);
    CHECK_EQ(as.back().data == std::vector<uint8_t>({7}), true);
    for (const auto* seen : {&as, &bs})
        for (auto k : {SessionEvent::Kind::Left, SessionEvent::Kind::RoomClosed, SessionEvent::Kind::Welcome})
            CHECK_EQ(std::count_if(seen->begin(), seen->end(), kind(k)), k == SessionEvent::Kind::Welcome);
}

TEST(session_leaves_the_room_once_its_game_stops_polling) {
    StubRoom room;
    Session a(room.url("/api/rooms/new"), hello());
    std::vector<SessionEvent> as, bs;
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Welcome)), true);
    Session b(room.url("/api/rooms/ABCD"), hello(), std::chrono::milliseconds(1500));
    CHECK_EQ(wait_for(b, bs, kind(SessionEvent::Kind::Open)), true);
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Open)), true);
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Left)), true);
    std::vector<SessionEvent> late = b.poll();
    CHECK_EQ(late.size() == 2 && late[0].kind == SessionEvent::Kind::Left && late[0].peer == "p0", true);
    CHECK_EQ(!late.empty() && late.back().kind == SessionEvent::Kind::RoomClosed, true);
    CHECK_EQ(late.back().text.find("stopped for more than 1.5 seconds") != std::string::npos, true);
}

TEST(session_that_keeps_polling_stays_in_the_room) {
    StubRoom room;
    Session a(room.url("/api/rooms/new"), hello());
    std::vector<SessionEvent> as, bs;
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Welcome)), true);
    Session b(room.url("/api/rooms/ABCD"), hello(), std::chrono::milliseconds(500));
    CHECK_EQ(wait_for(b, bs, kind(SessionEvent::Kind::Open)), true);
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Open)), true);
    for (auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2); std::chrono::steady_clock::now() < until;) {
        for (auto& e : b.poll()) bs.push_back(e);
        for (auto& e : a.poll()) as.push_back(e);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_EQ(std::none_of(bs.begin(), bs.end(), kind(SessionEvent::Kind::RoomClosed)), true);
    CHECK_EQ(std::none_of(as.begin(), as.end(), kind(SessionEvent::Kind::Left)), true);
}

TEST(session_is_refused_with_the_rooms_reason) {
    StubRoom room;
    Session a(room.url("/api/rooms/new"), hello("refuse-me"));
    std::vector<SessionEvent> as;
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::Refused)), true);
    CHECK_EQ(as.back().text == "the game differs", true);
}

TEST(session_reports_a_room_that_never_answers) {
    Session a("ws://127.0.0.1:1/api/rooms/new", hello());
    std::vector<SessionEvent> as;
    CHECK_EQ(wait_for(a, as, kind(SessionEvent::Kind::RoomClosed)), true);
}

TEST(session_keeps_the_ice_servers_libjuice_can_use) {
    const json servers = json::parse(R"([
        {"urls": ["stun:stun.cloudflare.com:3478"]},
        {"urls": ["turn:turn.cloudflare.com:3478?transport=udp", "turn:turn.cloudflare.com:53?transport=udp",
                  "turn:turn.cloudflare.com:3478?transport=tcp", "turns:turn.cloudflare.com:5349?transport=tcp",
                  "turn:turn.cloudflare.com:80"],
         "username": "u", "credential": "c"},
        {"urls": "stun:stun.example:19302"},
        {"urls": ["nonsense", "stun:no-port"]}
    ])");
    const auto got = session_ice_servers(servers);
    CHECK_EQ(got.size(), 4);
    if (got.size() != 4) return;
    CHECK_EQ(got[0].hostname == "stun.cloudflare.com" && got[0].port == 3478 && got[0].type == rtc::IceServer::Type::Stun, true);
    CHECK_EQ(got[1].port == 3478 && got[1].type == rtc::IceServer::Type::Turn && got[1].username == "u" && got[1].password == "c", true);
    CHECK_EQ(got[2].port == 80 && got[2].type == rtc::IceServer::Type::Turn, true);
    CHECK_EQ(got[3].hostname == "stun.example" && got[3].port == 19302, true);
}

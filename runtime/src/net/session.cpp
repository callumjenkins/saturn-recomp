// saturn-recomp runtime — a session over libdatachannel (session.h). The room's WebSocket carries the hello and each
// connection's offers, answers and candidates; the peer that joins last offers to every peer already there.
//
// A network that changes under a phone takes its connections with it, and libdatachannel cannot restart ICE. So a
// session whose room socket drops joins the room again as itself and offers every peer a new connection, and a
// connection that fails while the room is still there is offered again by the peer with the lower id. A peer
// not connected again within kComeBackFor is reported Left.
//
// libdatachannel calls back on threads of its own. A channel's message goes straight to the events; every other
// callback posts a task to the session's thread, which alone touches the links and the room's socket.
#include "session.h"
#include "ca_bundle.h"
#include "mbedtls_threads.h"
#include "session_ice.h"
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

using json = nlohmann::json;

namespace {

const char* const kLabels[] = {"pads", "line", "control"};
const int kChannels = 3;
const int kAllOpen = (1 << kChannels) - 1;
// The room's RESUME_MS, which keeps a peer's place while it comes back.
const auto kComeBackFor = std::chrono::seconds(15);
// Coming back, each try at the room's socket starts once the last has failed, and fails after kConnectFor.
const auto kRejoinEvery = std::chrono::seconds(1);
const auto kConnectFor = std::chrono::seconds(5);
// Unanswered for two of these, the room's socket closes: a dropped network is noticed within about 4 seconds.
const auto kPingEvery = std::chrono::seconds(2);

using Clock = std::chrono::steady_clock;

int channel_of(const std::string& label) {
    for (int i = 0; i < kChannels; ++i)
        if (label == kLabels[i]) return i;
    return -1;
}

// The kinds of the two candidates `pc` chose, such as "srflx to relay"; empty before it has chosen.
std::string route(rtc::PeerConnection& pc) {
    rtc::Candidate local, remote;
    if (!pc.getSelectedCandidatePair(&local, &remote)) return "";
    std::ostringstream out;
    out << local.type() << " to " << remote.type();
    return out.str();
}

}  // namespace

std::vector<rtc::IceServer> session_ice_servers(const json& servers) {
    std::vector<rtc::IceServer> out;
    if (!servers.is_array()) return out;
    for (const json& s : servers) {
        if (!s.is_object()) continue;
        json urls = s.value("urls", json::array());
        if (urls.is_string()) urls = json::array({urls});
        if (!urls.is_array()) continue;
        const std::string user = s.value("username", ""), password = s.value("credential", "");
        for (const json& u : urls) {
            if (!u.is_string()) continue;
            const std::string url = u.get<std::string>();
            const size_t colon = url.find(':'), query = url.find('?');
            const std::string scheme = url.substr(0, colon);
            const std::string where = url.substr(colon + 1, query == std::string::npos ? std::string::npos : query - colon - 1);
            const std::string options = query == std::string::npos ? "" : url.substr(query + 1);
            const size_t port_at = where.rfind(':');
            if (colon == std::string::npos || port_at == std::string::npos) continue;
            const std::string host = where.substr(0, port_at);
            const int port = std::atoi(where.c_str() + port_at + 1);
            if (port <= 0 || port > 65535 || port == 53) continue;
            if (scheme == "stun") out.emplace_back(host, (uint16_t)port);
            else if (scheme == "turn" && options.find("transport=tcp") == std::string::npos)
                out.emplace_back(host, (uint16_t)port, user, password, rtc::IceServer::RelayType::TurnUdp);
        }
    }
    return out;
}

struct Session::Impl {
    struct Link {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> channels[kChannels];
        int open = 0;   // a bit for each channel open
        std::optional<Clock::time_point> broken_since;   // while a new connection replaces a failed one
    };

    SessionHello hello;
    std::string url;
    std::shared_ptr<rtc::WebSocket> room;
    rtc::Configuration config;
    std::map<std::string, Link> links;
    bool room_closed = false;
    std::string self, code;                           // from the welcome
    std::optional<Clock::time_point> rejoining_since;  // while the room's socket is gone and the session comes back
    Clock::time_point rejoin_tried;
    std::chrono::milliseconds away_after{};
    std::atomic<std::chrono::steady_clock::rep> last_poll{};
    bool gone_away = false;

    std::mutex task_mutex;
    std::condition_variable woken;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::thread thread;

    std::mutex event_mutex;
    std::vector<SessionEvent> events;

    void post(std::function<void()> task) {
        std::scoped_lock lock(task_mutex);
        if (stopping) return;
        tasks.push_back(std::move(task));
        woken.notify_one();
    }

    void emit(SessionEvent e) {
        std::scoped_lock lock(event_mutex);
        events.push_back(std::move(e));
    }

    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(task_mutex);
                woken.wait_for(lock, std::chrono::milliseconds(250), [&] { return stopping || !tasks.empty(); });
                if (stopping) break;
                if (!tasks.empty()) {
                    task = std::move(tasks.front());
                    tasks.pop_front();
                }
            }
            if (task && !gone_away) {
                try {
                    task();
                } catch (const std::exception& e) {
                    emit({.kind = SessionEvent::Kind::RoomClosed, .text = std::string("the session failed: ") + e.what()});
                }
            }
            leave_if_away();
            if (!gone_away) come_back();
        }
        shut();
    }

    // Joins the room again while its socket is gone, and gives up on connections not back in time.
    void come_back() {
        const auto now = Clock::now();
        if (rejoining_since && now - *rejoining_since >= kComeBackFor) {
            rejoining_since.reset();
            close_room("the room's connection closed, and joining it again failed");
        } else if (rejoining_since && now - rejoin_tried >= kRejoinEvery &&
                   (!room || room->readyState() == rtc::WebSocket::State::Closed)) {
            rejoin_tried = now;
            start();
        }
        std::vector<std::string> lost;
        for (const auto& [id, link] : links)
            if (link.broken_since && now - *link.broken_since >= kComeBackFor) lost.push_back(id);
        for (const std::string& id : lost) drop(id, "its connection failed");
    }

    void leave_if_away() {
        using Clock = std::chrono::steady_clock;
        if (gone_away || Clock::now() - Clock::time_point(Clock::duration(last_poll.load())) < away_after) return;
        gone_away = true;
        for (const auto& [id, _] : links)
            emit({.kind = SessionEvent::Kind::Left, .peer = id, .text = "is no longer reached: this game left"});
        shut();
        room_closed = true;
        const auto seconds = std::chrono::duration_cast<std::chrono::duration<double>>(away_after).count();
        char why[96];
        std::snprintf(why, sizeof why, "the game stopped for more than %g seconds, so it left the room", seconds);
        emit({.kind = SessionEvent::Kind::RoomClosed, .text = why});
    }

    void start() {
        rtc::WebSocketConfiguration wc;
        wc.caCertificatePemFile = ca_bundle_pem();
        wc.pingInterval = kPingEvery;
        wc.maxOutstandingPings = 1;
        wc.connectionTimeout = kConnectFor;
        if (room) {
            room->resetCallbacks();
            room->close();
        }
        room = std::make_shared<rtc::WebSocket>(wc);
        auto socket = room.get();
        room->onOpen([this, socket] { post([this, socket] { if (socket == room.get()) say_hello(); }); });
        room->onMessage([this, socket](rtc::message_variant m) {
            if (auto* text = std::get_if<std::string>(&m)) post([this, socket, s = *text] { if (socket == room.get()) from_room(s); });
        });
        room->onError([this, socket](std::string why) { post([this, socket, why] { if (socket == room.get()) room_lost(why); }); });
        room->onClosed([this, socket] { post([this, socket] { if (socket == room.get()) room_lost("the room's connection closed"); }); });
        // Once in, the session comes back to the room it is in, whatever room the URL made.
        room->open(self.empty() ? url : url.substr(0, url.rfind('/') + 1) + code);
    }

    void say_hello() {
        json h = {{"type", "hello"}, {"pads", hello.pads},
                  {"terms", {{"product", hello.terms.product}, {"game", hello.terms.game},
                             {"runtime", hello.terms.runtime}, {"mode", hello.terms.mode}}}};
        if (!hello.token.empty()) h["token"] = hello.token;
        if (!hello.tester.empty()) h["tester"] = hello.tester;
        if (rejoining_since) h["resume"] = self;
        room->send(h.dump());
    }

    // A room joined comes back to; one never joined, or one that refused this peer, is closed.
    void room_lost(const std::string& why) {
        if (rejoining_since || room_closed) return;
        if (self.empty()) return close_room(why);
        rejoining_since = rejoin_tried = Clock::now();
        start();
    }

    void from_room(const std::string& text) {
        const json m = json::parse(text, nullptr, false);
        if (!m.is_object()) return;
        const std::string type = m.value("type", "");
        if (type == "welcome" && rejoining_since) {
            rejoining_since.reset();
            config.iceServers = session_ice_servers(m.value("iceServers", json::array()));
            std::map<std::string, bool> there;
            for (const json& p : m.value("peers", json::array()))
                if (p.is_object() && !p.value("id", "").empty()) there[p.value("id", "")] = true;
            std::vector<std::string> gone;
            for (const auto& [id, _] : links)
                if (!there.count(id)) gone.push_back(id);
            for (const std::string& id : gone) drop(id, "left the room");
            for (const json& p : m.value("peers", json::array())) {
                const std::string id = p.value("id", "");
                if (links.count(id)) reconnect(id, true);
                else if (joined(p)) connect(id, true);
            }
        } else if (type == "welcome") {
            config.iceServers = session_ice_servers(m.value("iceServers", json::array()));
            self = m.value("id", "");
            code = m.value("code", "");
            emit({.kind = SessionEvent::Kind::Welcome, .peer = self, .text = code});
            for (const json& p : m.value("peers", json::array())) {
                if (!joined(p)) continue;
                connect(p.value("id", ""), true);
            }
        } else if (type == "joined") {
            const json p = m.value("peer", json::object());
            const std::string id = p.value("id", "");
            if (links.count(id)) reconnect(id, false);   // it came back, and offers a new connection
            else if (joined(p)) connect(id, false);
        } else if (type == "signal") {
            signalled(m.value("from", ""), m.value("data", json::object()));
        } else if (type == "left") {
            drop(m.value("id", ""), "left the room");
        } else if (type == "refused" && rejoining_since) {
            rejoining_since.reset();
            std::vector<std::string> all;
            for (const auto& [id, _] : links) all.push_back(id);
            for (const std::string& id : all) drop(id, "is no longer reached: this game could not join the room again");
            close_room("joining again, " + m.value("reason", ""));
            room->close();
        } else if (type == "refused") {
            emit({.kind = SessionEvent::Kind::Refused, .text = m.value("reason", "")});
            room_closed = true;
            room->close();
        }
    }

    // A peer in a welcome or a join, announced; false for one with no id.
    bool joined(const json& p) {
        if (!p.is_object() || p.value("id", "").empty()) return false;
        emit({.kind = SessionEvent::Kind::Joined, .peer = p.value("id", ""), .name = p.value("name", ""), .pads = p.value("pads", 0)});
        return true;
    }

    void connect(const std::string& id, bool offer) {
        auto pc = std::make_shared<rtc::PeerConnection>(config);
        pc->onLocalDescription([this, id](rtc::Description d) {
            post([this, id, type = d.typeString(), sdp = std::string(d)] {
                signal(id, {{"description", {{"type", type}, {"sdp", sdp}}}});
            });
        });
        pc->onLocalCandidate([this, id](rtc::Candidate c) {
            post([this, id, candidate = std::string(c), mid = c.mid()] {
                signal(id, {{"candidate", {{"candidate", candidate}, {"mid", mid}}}});
            });
        });
        auto raw = pc.get();
        pc->onStateChange([this, id, raw](rtc::PeerConnection::State s) {
            if (s == rtc::PeerConnection::State::Failed) post([this, id, raw] { failed(id, raw); });
        });
        pc->onDataChannel([this, id](std::shared_ptr<rtc::DataChannel> dc) { post([this, id, dc] { attach(id, dc); }); });
        links[id].pc = pc;
        if (!offer) return;
        for (int i = 0; i < kChannels; ++i) {
            rtc::DataChannelInit init;
            if (i == (int)Channel::Pads) {
                init.reliability.unordered = true;
                init.reliability.maxRetransmits = 0;
            }
            attach(id, pc->createDataChannel(kLabels[i], init));
        }
    }

    void attach(const std::string& id, const std::shared_ptr<rtc::DataChannel>& dc) {
        const int c = channel_of(dc->label());
        auto it = links.find(id);
        if (c < 0 || it == links.end()) return;
        it->second.channels[c] = dc;
        dc->onOpen([this, id, c] { post([this, id, c] { opened(id, c); }); });
        dc->onMessage([this, id, c](rtc::message_variant m) {
            auto* bytes = std::get_if<rtc::binary>(&m);
            if (!bytes) return;
            const auto* p = reinterpret_cast<const uint8_t*>(bytes->data());
            emit({.kind = SessionEvent::Kind::Message, .peer = id, .channel = (Channel)c, .data = {p, p + bytes->size()}});
        });
        if (dc->isOpen()) opened(id, c);
    }

    void opened(const std::string& id, int c) {
        auto it = links.find(id);
        if (it == links.end() || it->second.open == kAllOpen) return;
        it->second.open |= 1 << c;
        if (it->second.open != kAllOpen) return;
        it->second.broken_since.reset();
        emit({.kind = SessionEvent::Kind::Open, .peer = id, .text = route(*it->second.pc)});
    }

    // `pc`'s connection to `id` failed. The lower id offers its replacement while the room is there to carry it.
    void failed(const std::string& id, rtc::PeerConnection* pc) {
        auto it = links.find(id);
        if (it == links.end() || it->second.pc.get() != pc || it->second.broken_since) return;
        it->second.broken_since = Clock::now();
        if (self < id && room && room->isOpen()) reconnect(id, true);
    }

    // A new connection to `id` in place of its link, offered by this side or awaiting the other's offer.
    void reconnect(const std::string& id, bool offer) {
        Link& old = links[id];
        const auto since = old.broken_since.value_or(Clock::now());
        if (old.pc) close_link(old);
        links[id] = Link{.broken_since = since};
        connect(id, offer);
    }

    void signalled(const std::string& from, const json& data) {
        auto it = links.find(from);
        if (it == links.end() || !data.is_object()) return;
        if (data.contains("description")) {
            const json& d = data["description"];
            // An offer to a connection that already has one is the other side replacing a failed connection.
            if (d.value("type", "") == "offer" && it->second.pc->remoteDescription()) {
                reconnect(from, false);
                it = links.find(from);
            }
            it->second.pc->setRemoteDescription(rtc::Description(d.value("sdp", ""), d.value("type", "")));
        } else if (data.contains("candidate")) {
            const json& c = data["candidate"];
            it->second.pc->addRemoteCandidate(rtc::Candidate(c.value("candidate", ""), c.value("mid", "")));
        }
    }

    void signal(const std::string& to, json data) {
        if (room && room->isOpen()) room->send(json{{"type", "signal"}, {"to", to}, {"data", std::move(data)}}.dump());
    }

    void drop(const std::string& id, const std::string& how) {
        auto it = links.find(id);
        if (it == links.end()) return;
        close_link(it->second);
        links.erase(it);
        emit({.kind = SessionEvent::Kind::Left, .peer = id, .text = how});
    }

    void close_room(const std::string& why) {
        if (room_closed) return;
        room_closed = true;
        emit({.kind = SessionEvent::Kind::RoomClosed, .text = why});
    }

    void sent(const std::string& peer, Channel channel, const std::vector<uint8_t>& data) {
        for (auto& [id, link] : links) {
            if (!peer.empty() && id != peer) continue;
            auto& dc = link.channels[(int)channel];
            if (dc && dc->isOpen()) dc->send(reinterpret_cast<const std::byte*>(data.data()), data.size());
        }
    }

    static void close_link(Link& link) {
        for (auto& dc : link.channels)
            if (dc) dc->resetCallbacks();
        link.pc->resetCallbacks();
        link.pc->close();
    }

    // On the session's thread, as it ends: nothing calls back once this returns. The bye tells the room this peer
    // is not coming back.
    void shut() {
        for (auto& [id, link] : links) close_link(link);
        links.clear();
        if (room) {
            room->resetCallbacks();
            if (room->isOpen()) room->send(json{{"type", "bye"}}.dump());
            room->close();
            room.reset();
        }
    }

    // For tests: the network gone at once, with no bye and no goodbyes to the peers.
    void sever() {
        if (room) room->forceClose();
        for (auto& [id, link] : links) {
            close_link(link);
            link.broken_since = Clock::now();
        }
    }
};

Session::Session(const std::string& url, const SessionHello& hello, std::chrono::milliseconds away_after)
    : impl_(std::make_unique<Impl>()) {
    mbedtls_threads_start();
    impl_->hello = hello;
    impl_->url = url;
    impl_->away_after = away_after;
    impl_->last_poll = std::chrono::steady_clock::now().time_since_epoch().count();
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    impl_->post([impl = impl_.get()] { impl->start(); });
}

Session::~Session() {
    {
        std::scoped_lock lock(impl_->task_mutex);
        impl_->stopping = true;
        impl_->tasks.clear();
        impl_->woken.notify_one();
    }
    impl_->thread.join();
}

std::vector<SessionEvent> Session::poll() {
    impl_->last_poll = std::chrono::steady_clock::now().time_since_epoch().count();
    std::scoped_lock lock(impl_->event_mutex);
    return std::exchange(impl_->events, {});
}

void Session::sever() {
    impl_->post([impl = impl_.get()] { impl->sever(); });
}

void Session::send(const std::string& peer, Channel channel, std::vector<uint8_t> data) {
    impl_->post([impl = impl_.get(), peer, channel, data = std::move(data)] { impl->sent(peer, channel, data); });
}

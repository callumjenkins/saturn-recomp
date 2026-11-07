// saturn-recomp runtime — a session over libdatachannel (session.h). The room's WebSocket carries the hello and each
// connection's offers, answers and candidates; the peer that joins last offers to every peer already there.
//
// libdatachannel calls back on threads of its own. A channel's message goes straight to the events; every other
// callback posts a task to the session's thread, which alone touches the links and the room's socket.
#include "session.h"
#include "cacert.h"
#include "mbedtls_threads.h"
#include "session_ice.h"
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <condition_variable>
#include <cstdlib>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

using json = nlohmann::json;

namespace {

const char* const kLabels[] = {"pads", "line", "control"};
const int kChannels = 3;
const int kAllOpen = (1 << kChannels) - 1;

int channel_of(const std::string& label) {
    for (int i = 0; i < kChannels; ++i)
        if (label == kLabels[i]) return i;
    return -1;
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
    };

    SessionHello hello;
    std::string url;
    std::shared_ptr<rtc::WebSocket> room;
    rtc::Configuration config;
    std::map<std::string, Link> links;
    bool room_closed = false;

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
                woken.wait(lock, [&] { return stopping || !tasks.empty(); });
                if (tasks.empty()) break;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            try {
                task();
            } catch (const std::exception& e) {
                emit({.kind = SessionEvent::Kind::RoomClosed, .text = std::string("the session failed: ") + e.what()});
            }
        }
        shut();
    }

    void start() {
        rtc::WebSocketConfiguration wc;
        wc.caCertificatePemFile = std::string(reinterpret_cast<const char*>(cacert_pem), sizeof cacert_pem);
        room = std::make_shared<rtc::WebSocket>(wc);
        room->onOpen([this] { post([this] { say_hello(); }); });
        room->onMessage([this](rtc::message_variant m) {
            if (auto* text = std::get_if<std::string>(&m)) post([this, s = *text] { from_room(s); });
        });
        room->onError([this](std::string why) { post([this, why] { close_room(why); }); });
        room->onClosed([this] { post([this] { close_room("the room's connection closed"); }); });
        room->open(url);
    }

    void say_hello() {
        json h = {{"type", "hello"}, {"pads", hello.pads},
                  {"terms", {{"product", hello.terms.product}, {"game", hello.terms.game},
                             {"runtime", hello.terms.runtime}, {"mode", hello.terms.mode}}}};
        if (!hello.token.empty()) h["token"] = hello.token;
        if (!hello.tester.empty()) h["tester"] = hello.tester;
        room->send(h.dump());
    }

    void from_room(const std::string& text) {
        const json m = json::parse(text, nullptr, false);
        if (!m.is_object()) return;
        const std::string type = m.value("type", "");
        if (type == "welcome") {
            config.iceServers = session_ice_servers(m.value("iceServers", json::array()));
            emit({.kind = SessionEvent::Kind::Welcome, .text = m.value("code", "")});
            for (const json& p : m.value("peers", json::array())) {
                if (!joined(p)) continue;
                connect(p.value("id", ""), true);
            }
        } else if (type == "joined") {
            const json p = m.value("peer", json::object());
            if (joined(p)) connect(p.value("id", ""), false);
        } else if (type == "signal") {
            signalled(m.value("from", ""), m.value("data", json::object()));
        } else if (type == "left") {
            drop(m.value("id", ""), "left the room");
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
        pc->onStateChange([this, id](rtc::PeerConnection::State s) {
            if (s == rtc::PeerConnection::State::Failed) post([this, id] { drop(id, "its connection failed"); });
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
        if (it->second.open == kAllOpen) emit({.kind = SessionEvent::Kind::Open, .peer = id});
    }

    void signalled(const std::string& from, const json& data) {
        auto it = links.find(from);
        if (it == links.end() || !data.is_object()) return;
        if (data.contains("description")) {
            const json& d = data["description"];
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

    // On the session's thread, as it ends: nothing calls back once this returns.
    void shut() {
        for (auto& [id, link] : links) close_link(link);
        links.clear();
        if (room) {
            room->resetCallbacks();
            room->close();
            room.reset();
        }
    }
};

Session::Session(const std::string& url, const SessionHello& hello) : impl_(std::make_unique<Impl>()) {
    mbedtls_threads_start();
    impl_->hello = hello;
    impl_->url = url;
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
    std::scoped_lock lock(impl_->event_mutex);
    return std::exchange(impl_->events, {});
}

void Session::send(const std::string& peer, Channel channel, std::vector<uint8_t> data) {
    impl_->post([impl = impl_.get(), peer, channel, data = std::move(data)] { impl->sent(peer, channel, data); });
}

// saturn-recomp runtime — --sync: the peers in a playtest room play one game, each running the same machine with
// every peer's pads (lockstep.h). The room's creator waits at the first VBlank until --sync's number of players are
// connected, then starts the game for all of them with each peer's slots and the input delay. Each VBlank, a peer
// sends its own pads and waits for everyone's. Every second the peers compare hashes of work RAM, so a machine that
// falls out of step is reported.
#include "saturn.h"
#include "host.h"
#include "lockstep.h"
#include "session.h"
#include <saturn/sh2.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

const auto kLostAfter = std::chrono::seconds(10);   // as long as a session waits for a game paused in the background
const auto kWaitShown = std::chrono::seconds(1);
const auto kResend = std::chrono::milliseconds(4);
const auto kPingEvery = std::chrono::milliseconds(200);
const size_t kPingsNeeded = 5;
const uint64_t kHashEvery = 60;
const uint64_t kFramesKept = 128;

struct Peer {
    std::string name;
    int pads = 1;
    bool open = false;
    std::vector<uint64_t> rtts;       // round trips to it, in ns, while waiting to start
};

std::unique_ptr<Lockstep> g_ls;
std::string g_self, g_code;
std::vector<std::string> g_order;     // the other peers, in the order they joined
std::map<std::string, Peer> g_peers;
std::string g_ended;                  // why the game is no longer played together
std::string g_waiting_for;            // the member a long wait is for
bool g_start_asked;
uint16_t g_pads[kLockstepSlots];
Clock::time_point g_pinged;

uint64_t now_ns() { return (uint64_t)Clock::now().time_since_epoch().count(); }

bool creator() { return g_cfg.session.find("/api/rooms/new") != std::string::npos; }

void send(const std::string& peer, Channel channel, std::vector<uint8_t> m) {
    if (Session* s = online_session()) s->send(peer, channel, std::move(m));
}

// To every member but this one and those gone.
void to_members(Channel channel, const std::vector<uint8_t>& m) {
    for (int i = 0; i < (int)g_ls->members().size(); ++i)
        if (i != g_ls->self() && !g_ls->gone(i)) send(g_ls->members()[i].id, channel, m);
}

void end(const std::string& why) {
    if (!g_ended.empty()) return;
    g_ended = why;
    sat_note("sync: %s", why.c_str());
}

std::string name(const std::string& peer) {
    auto it = g_peers.find(peer);
    return it == g_peers.end() || it->second.name.empty() ? peer : it->second.name;
}

void lose(int member, const char* how) {
    std::vector<uint8_t> report = g_ls->lose(member);
    if (report.empty()) return;
    sat_note("sync: %s %s", name(g_ls->members()[member].id).c_str(), how);
    to_members(Channel::Control, report);
}

uint64_t median(std::vector<uint64_t> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// The players whose channels are open, this one included.
int players_in() {
    int in = 1;
    for (const std::string& id : g_order) in += g_peers[id].open;
    return in;
}

// The creator's start, once its player has asked for it or --sync's number are in, and every open channel's round
// trip is measured: each peer's slots one after the other, this one's first.
std::optional<LockstepStart> ready_to_start() {
    std::vector<std::string> open;
    for (const std::string& id : g_order)
        if (g_peers[id].open && g_peers[id].rtts.size() >= kPingsNeeded) open.push_back(id);
    const int ready = (int)open.size() + 1;
    const bool asked = g_start_asked || (g_cfg.sync >= 2 && ready >= g_cfg.sync);
    if (ready < 2 || !asked || ready < players_in()) return std::nullopt;
    LockstepStart s{.frame = sat_vblanks(), .delay = g_cfg.sync_delay};
    uint64_t rtt = 0;
    int slot = 0;
    s.members.push_back({g_self, slot, g_cfg.sync_pads});
    slot += g_cfg.sync_pads;
    for (const std::string& id : open) {
        if (slot + g_peers[id].pads > kLockstepSlots) break;
        s.members.push_back({id, slot, g_peers[id].pads});
        slot += g_peers[id].pads;
        rtt = std::max(rtt, median(g_peers[id].rtts));
    }
    if (!s.delay) s.delay = lockstep_delay_for(rtt);
    return s;
}

void begin(const LockstepStart& s) {
    int self = -1;
    for (int i = 0; i < (int)s.members.size(); ++i)
        if (s.members[i].id == g_self) self = i;
    if (self < 0) return end("the game started without this player");
    if (s.frame != sat_vblanks()) return end("the game started at another VBlank");
    g_ls = std::make_unique<Lockstep>(s, self);
    sat_note("sync: the game starts at VBlank %llu with %zu players, the pads %d frames behind",
             (unsigned long long)s.frame, s.members.size(), s.delay);
    for (const LockstepMember& m : s.members)
        sat_note("sync: %s has pads %d to %d", m.id == g_self ? "this game" : name(m.id).c_str(), m.first + 1,
                 m.first + m.count);
}

void heard_control(const SessionEvent& e) {
    const std::vector<uint8_t>& m = e.data;
    if (m.empty()) return;
    switch ((LockstepKind)m[0]) {
    case LockstepKind::Start:
        if (auto s = lockstep_read_start(m); s && !g_ls && g_ended.empty()) begin(*s);
        break;
    case LockstepKind::Ping:
        if (auto t = lockstep_read_time(m)) send(e.peer, Channel::Control, lockstep_time_message(LockstepKind::Pong, *t));
        break;
    case LockstepKind::Pong:
        if (auto t = lockstep_read_time(m); t && g_peers.count(e.peer)) g_peers[e.peer].rtts.push_back(now_ns() - *t);
        break;
    case LockstepKind::Late:
        if (!g_ls) end("the game had already started");
        break;
    case LockstepKind::Lost:
        if (g_ls && g_ls->member_of(e.peer) >= 0) {
            Lockstep::LostHeard heard = g_ls->hear_lost(g_ls->member_of(e.peer), m);
            if (heard.self) end("the other players lost this game's connection");
            if (!heard.report.empty()) to_members(Channel::Control, heard.report);
        }
        break;
    case LockstepKind::Hash:
        if (g_ls && g_ls->member_of(e.peer) >= 0) g_ls->hear_hash(g_ls->member_of(e.peer), m);
        break;
    }
}

void ping_open_peers() {
    if (Clock::now() - g_pinged < kPingEvery) return;
    g_pinged = Clock::now();
    for (const std::string& id : g_order)
        if (g_peers[id].open) send(id, Channel::Control, lockstep_time_message(LockstepKind::Ping, now_ns()));
}

// Polls the room until `done`, re-showing the picture every frame's time so the window answers.
template <class Done> void wait(const std::function<void()>& idle, Done done) {
    auto shown = Clock::now();
    for (;;) {
        online_poll();
        if (done() || !g_ended.empty()) return;
        if (Clock::now() - shown >= std::chrono::milliseconds(16)) {
            idle();
            shown = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// After the game stops being played together: this peer's own pads in its slots, and the rest idle.
void alone() {
    uint16_t local[kLockstepSlots] = {};
    const LockstepMember& own = g_ls->members()[g_ls->self()];
    smpc_local_pads(local, own.count);
    std::fill(g_pads, g_pads + kLockstepSlots, 0);
    std::copy(local, local + own.count, g_pads + own.first);
}

void send_pads() {
    for (int i = 0; i < (int)g_ls->members().size(); ++i)
        if (i != g_ls->self() && !g_ls->gone(i)) send(g_ls->members()[i].id, Channel::Pads, g_ls->pads_message(i));
}

}  // namespace

bool online_sync_playing() { return g_ls != nullptr; }

uint16_t online_sync_pad(int slot) { return slot < kLockstepSlots ? g_pads[slot] : 0; }

bool online_sync_plugged(int slot) { return g_ls && slot < kLockstepSlots && g_ls->owned(slot); }

void online_sync_hear(const SessionEvent& e) {
    if (g_cfg.sync < 0) return;
    switch (e.kind) {
    case SessionEvent::Kind::Welcome:
        g_self = e.peer;
        g_code = e.text;
        break;
    case SessionEvent::Kind::Joined:
        if (!g_peers.count(e.peer)) g_order.push_back(e.peer);
        g_peers[e.peer] = {.name = e.name, .pads = e.pads};
        break;
    case SessionEvent::Kind::Open:
        g_peers[e.peer].open = true;
        if (g_ls && g_ls->member_of(e.peer) < 0 && creator()) send(e.peer, Channel::Control, {(uint8_t)LockstepKind::Late});
        break;
    case SessionEvent::Kind::Left:
        if (g_ls && g_ls->member_of(e.peer) >= 0) lose(g_ls->member_of(e.peer), e.text.c_str());
        g_order.erase(std::remove(g_order.begin(), g_order.end(), e.peer), g_order.end());
        g_peers.erase(e.peer);
        break;
    case SessionEvent::Kind::Message:
        if (e.channel == Channel::Control) heard_control(e);
        else if (e.channel == Channel::Pads && g_ls && g_ls->member_of(e.peer) >= 0)
            g_ls->hear_pads(g_ls->member_of(e.peer), e.data);
        break;
    case SessionEvent::Kind::Refused:
    case SessionEvent::Kind::RoomClosed:
        if (!g_ls) end(e.text);
        break;
    }
}

void online_sync_vblank(const std::function<void()>& idle) {
    if (g_cfg.sync < 0) return;
    if (!g_ended.empty()) {
        if (g_ls) alone();
        return;
    }
    const uint64_t frame = sat_vblanks();
    if (!g_ls) {
        wait(idle, [&] {
            if (creator()) {
                ping_open_peers();
                if (auto s = ready_to_start()) {
                    begin(*s);
                    to_members(Channel::Control, lockstep_start_message(*s));
                }
            }
            return g_ls != nullptr;
        });
        if (!g_ls) return;
    }
    uint16_t local[kLockstepSlots] = {};
    smpc_local_pads(local, g_ls->members()[g_ls->self()].count);
    g_ls->local(frame, local);
    send_pads();
    const auto began = Clock::now();
    auto sent = began;
    wait(idle, [&] {
        if (g_ls->ready(frame)) return true;
        const auto waited = Clock::now() - began;
        const std::vector<int> missing = g_ls->waiting_on(frame);
        g_waiting_for = waited >= kWaitShown && !missing.empty() ? name(g_ls->members()[missing[0]].id) : "";
        if (waited >= kLostAfter)
            for (int m : missing) lose(m, "sent no pads for 10 seconds");
        if (Clock::now() - sent >= kResend) {
            send_pads();
            sent = Clock::now();
        }
        return false;
    });
    g_waiting_for.clear();
    if (!g_ended.empty()) return alone();
    host_pace_hold((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count());
    g_ls->pads(frame, g_pads);
    if (frame % kHashEvery == 0) {
        const uint64_t hash = lockstep_hash(g_wram_l, 0x100000, lockstep_hash(g_wram_h, 0x100000));
        to_members(Channel::Control, g_ls->hash_message(frame, hash));
    }
    if (auto parted = g_ls->parted())
        end("this game and " + name(g_ls->members()[g_ls->parted_with()].id) + "'s fell out of step by VBlank " +
            std::to_string(*parted));
    if (frame > kFramesKept) g_ls->forget_before(frame - kFramesKept);
}

OnlineCard online_sync_card() {
    if (!g_ended.empty()) return {"Online: " + g_ended};
    if (!g_waiting_for.empty()) return {"Waiting for " + g_waiting_for};
    if (g_ls) return {};
    std::string text = g_code.empty() ? "Joining the room" : "Room " + g_code;
    if (!creator()) return {text + ": waiting for the game to start"};
    const int in = players_in();
    text += ": " + std::to_string(in) + (g_cfg.sync >= 2 ? " of " + std::to_string(g_cfg.sync) : "") +
            (in == 1 ? " player" : " players");
    if (g_start_asked) return {text + ", starting"};
    if (in < 2) return {text};
    return {text + ". Start: play", true};
}

void online_sync_start() {
    if (creator() && !g_ls && players_in() >= 2) g_start_asked = true;
}

// saturn-recomp runtime — --session: the run joins a playtest room (net/session.h) once the boot has put the disc's
// IP.BIN in work RAM, and notes who comes and goes. With --modem session, the modem's line reaches the first peer
// whose channels open (online_room.h), and what the line hears goes to OUT/line.txt, which --line-in plays to the
// line again in a replay. The window shows what the room has to tell the player (online_card).
#include "saturn.h"
#include "line_log.h"
#include "modem.h"
#include "modem_session.h"
#include "online_room.h"
#include "saturn_build_id.h"
#include "session.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>

namespace {

std::unique_ptr<Session> g_session;
SessionLine* g_line;            // the modem's, which owns it
OnlineRoom g_room;
FILE* g_line_out;
std::vector<LineHeard> g_line_in;
size_t g_line_in_pos;

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// Both ports' pads, or a multitap's six and the other port's one, or two multitaps' twelve, of which a room takes ten.
int pads() {
    if (g_cfg.multitap == 1) return 7;
    if (g_cfg.multitap == 2) return 10;
    return 2;
}

SessionHello hello() {
    return {.token = env("SATURN_PLAYTEST_TOKEN"),
            .tester = env("SATURN_PLAYTEST_TESTER"),
            .terms = {.product = bios_product(g_wram_h + 0x2000),
                      .game = SATURN_GAME_BUILD,
                      .runtime = SATURN_RUNTIME_BUILD,
                      .mode = g_cfg.modem.empty() ? "session" : "classic"},
            .pads = pads()};
}

// --steer-again's PROGRAM and SCRIPT.
std::pair<std::string, std::string> steer_again() {
    size_t colon = g_cfg.steer_again.find(':');
    if (colon == std::string::npos) return {};
    return {g_cfg.steer_again.substr(0, colon), g_cfg.steer_again.substr(colon + 1)};
}

void line_hears(const LineHeard& h) {
    if (h.message.empty()) g_line->reachable(h.reachable);
    else g_line->heard(h.message);
    if (!g_line_out) return;
    std::fprintf(g_line_out, "%s\n", line_log_entry(h).c_str());
    std::fflush(g_line_out);   // a session is sent while it plays
}

std::vector<LineHeard> read_line(const std::string& path) {
    std::ifstream in(path);
    if (!in) sat_fatal("--line-in: cannot read %s", path.c_str());
    std::vector<LineHeard> out;
    for (std::string text; std::getline(in, text);) {
        std::optional<LineHeard> h = line_log_read(text);
        if (!h) sat_fatal("--line-in %s: \"%s\" is not TIME reachable 0|1 or TIME heard HEX", path.c_str(), text.c_str());
        out.push_back(std::move(*h));
    }
    return out;
}

void hear(const SessionEvent& e) {
    switch (g_room.hear(e)) {
    case OnlineRoom::Far::Came:
        if (g_line) line_hears({sat_now(), true, {}});
        break;
    case OnlineRoom::Far::Went:
        if (g_line) line_hears({sat_now(), false, {}});
        break;
    case OnlineRoom::Far::Unchanged:
        if (e.kind == SessionEvent::Kind::Message && e.peer == g_room.far() && e.channel == Channel::Line && g_line)
            line_hears({sat_now(), true, e.data});
        break;
    }
}

void note(const SessionEvent& e) {
    switch (e.kind) {
    case SessionEvent::Kind::Welcome: sat_note("session: in room %s", e.text.c_str()); break;
    case SessionEvent::Kind::Joined:
        sat_note("session: %s is in the room as %s, with %d pads", e.peer.c_str(), e.name.c_str(), e.pads);
        break;
    case SessionEvent::Kind::Open: sat_note("session: connected to %s", e.peer.c_str()); break;
    case SessionEvent::Kind::Message: break;
    case SessionEvent::Kind::Left: sat_note("session: %s %s", e.peer.c_str(), e.text.c_str()); break;
    case SessionEvent::Kind::Refused: sat_note("session: refused: %s", e.text.c_str()); break;
    case SessionEvent::Kind::RoomClosed: sat_note("session: the room: %s", e.text.c_str()); break;
    }
}

}  // namespace

std::unique_ptr<ModemLine> online_modem_line(std::string& error) {
    if (g_cfg.session.empty() == g_cfg.line_in.empty()) {
        error = "a session line needs --session URL, or --line-in FILE in a replay, and not both";
        return nullptr;
    }
    if (!g_cfg.line_in.empty()) g_line_in = read_line(g_cfg.line_in);
    else if (!g_cfg.out.empty() && !(g_line_out = std::fopen((g_cfg.out + "/line.txt").c_str(), "w")))
        sat_fatal("--modem session: cannot write %s/line.txt", g_cfg.out.c_str());
    auto line = std::make_unique<SessionLine>([](std::vector<uint8_t> m) {
        if (g_session && !g_room.far().empty()) g_session->send(g_room.far(), Channel::Line, std::move(m));
    });
    g_line = line.get();
    return line;
}

void online_poll() {
    if (g_line && !g_cfg.line_in.empty()) {
        for (; g_line_in_pos < g_line_in.size() && g_line_in[g_line_in_pos].at <= sat_now(); ++g_line_in_pos)
            line_hears(g_line_in[g_line_in_pos]);
        return;
    }
    if (g_cfg.session.empty() || sat_vblanks() == 0) return;
    if (!g_session) {
        if (!g_cfg.steer_again.empty() && steer_again().first.empty())
            sat_fatal("--steer-again: %s is not PROGRAM:SCRIPT", g_cfg.steer_again.c_str());
        smpc_parse_script(steer_again().second);   // a fault in it now rather than when the player takes the offer
        g_session = std::make_unique<Session>(g_cfg.session, hello());
    }
    for (const SessionEvent& e : g_session->poll()) {
        hear(e);
        note(e);
    }
    if (g_line) g_line->flush();
    if (g_modem && g_modem->online()) g_room.called();
}

void online_finish() {
    if (g_line_out) std::fclose(g_line_out);
    g_line_out = nullptr;
    g_line = nullptr;
    g_session.reset();
}

void online_program_start(const char* module) {
    if (g_cfg.session.empty() || steer_again().first != module) return;
    if (g_room.program_started(sat_vblanks())) sat_note("session: another match is offered");
}

OnlineCard online_card() {
    if (g_cfg.session.empty()) return {};
    return g_room.card();
}

void online_again() {
    std::optional<uint64_t> offered_at = g_room.take_offer();
    if (!offered_at) return;
    std::vector<PadStep> steps = smpc_parse_script(steer_again().second);
    if (steps.empty()) return;
    smpc_steer(steps, online_again_from(*offered_at, steps.front().vblank, sat_vblanks()));
    sat_note("session: another match taken at VBlank %llu", (unsigned long long)sat_vblanks());
}

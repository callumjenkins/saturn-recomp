// saturn-recomp runtime — --session: the run joins a playtest room (net/session.h) once the boot has put the disc's
// IP.BIN in work RAM, and notes who comes and goes. Nothing in the machine plays through the session yet.
#include "saturn.h"
#include "session.h"
#include <cstdlib>
#include <memory>

namespace {

std::unique_ptr<Session> g_session;
uint64_t g_polled_vblank = ~0ull;

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

void note(const SessionEvent& e) {
    switch (e.kind) {
    case SessionEvent::Kind::Welcome: sat_note("session: in room %s", e.text.c_str()); break;
    case SessionEvent::Kind::Joined: sat_note("session: %s is in the room as %s, with %d pads", e.peer.c_str(), e.name.c_str(), e.pads); break;
    case SessionEvent::Kind::Open: sat_note("session: connected to %s", e.peer.c_str()); break;
    case SessionEvent::Kind::Message: break;
    case SessionEvent::Kind::Left: sat_note("session: %s %s", e.peer.c_str(), e.text.c_str()); break;
    case SessionEvent::Kind::Refused: sat_note("session: refused: %s", e.text.c_str()); break;
    case SessionEvent::Kind::RoomClosed: sat_note("session: the room: %s", e.text.c_str()); break;
    }
}

}  // namespace

void online_poll() {
    const uint64_t vblank = sat_vblanks();
    if (g_cfg.session.empty() || vblank == 0 || vblank == g_polled_vblank) return;
    g_polled_vblank = vblank;
    if (!g_session) g_session = std::make_unique<Session>(g_cfg.session, hello());
    for (const SessionEvent& e : g_session->poll()) note(e);
}

void online_finish() { g_session.reset(); }

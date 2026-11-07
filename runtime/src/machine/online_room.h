// saturn-recomp runtime — what a run in a playtest room knows of it (online.cpp): who is there, the far peer the
// modem's line reaches, the offer of another match, and what the window's card says.
#pragma once
#include "saturn.h"
#include "session.h"
#include <map>
#include <optional>
#include <string>

class OnlineRoom {
public:
    enum class Far { Unchanged, Came, Went };

    // What `e` did to the far peer: the first peer whose channels opened, until it leaves.
    Far hear(const SessionEvent& e);
    const std::string& far() const { return far_; }
    void called() { called_ = true; }   // a call has connected
    // --steer-again's program has started, at VBlank `now`. True when that offers another match: from its second
    // start, the first being the one --steer goes through, while the far peer is there.
    bool program_started(uint64_t now);
    std::optional<uint64_t> take_offer();   // the VBlank the offer was made at, if there is one
    OnlineCard card() const;

private:
    std::string name(const std::string& peer) const;

    std::string far_;
    std::string code_;            // the room's, once in it
    std::string trouble_;         // why the room refused the run or closed
    std::string gone_;            // the name of the far peer that left
    std::map<std::string, std::string> names_;   // each other peer's tester name, by id, while it is in the room
    bool called_ = false;         // a call has connected since the far peer came, or since the last offer was taken
    bool offered_ = false;
    uint64_t offered_at_ = 0;
    int program_starts_ = 0;
};

// Where a taken offer's script starts: its VBlanks count from the program's start, so taken after its first
// press was due, the script moves later and that press comes at `now`.
uint64_t online_again_from(uint64_t offered_at, uint64_t first_press, uint64_t now);

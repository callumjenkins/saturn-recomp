// saturn-recomp runtime — pads in lockstep (online_sync.cpp): every peer in a room runs the same machine, and each
// VBlank's pads are the same on every peer, from the slots each peer owns.
//
// A peer's pads sampled at VBlank N apply at N + delay everywhere, so the delay covers the time they take to arrive.
// No peer runs VBlank N until it has every peer's pads for it. Each pads message holds every frame the other peer
// has not yet acknowledged, so a lost message costs nothing once the next arrives.
//
// A peer that leaves has its pads go idle from a frame the rest agree on: each of them reports the last of its frames
// it holds, with the frames before that, and every peer takes the latest report and fills the gap from the reports.
// No peer can have run a frame later than the frames it holds, so none has run one the agreement changes.
//
// With rollback (`ahead` above 0), a peer runs a frame before every pad for it is here, guessing each missing one as
// that member's last, and keeps the pads each frame ran with. When a pad comes that differs from the guess, the frames
// from the first wrong one run again (online_sync.cpp). Each pads message also says the sender's VBlank and how far it
// thinks it is from the receiver, so the pair can tell which of them is ahead with neither knowing the round trip.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

const int kLockstepSlots = 12;        // smpc.cpp's pads: two multitaps' worth

struct LockstepMember {
    std::string id;                   // the room's peer id
    int first = 0, count = 1;         // the game's pad slots it owns
};

// What starts a lockstep game: the room's creator sends it to every other member.
struct LockstepStart {
    uint64_t frame = 0;               // the first VBlank all members run together
    int delay = 2;
    int ahead = 0;                    // frames a member may run past the last one with every pad; 0: none
    std::vector<LockstepMember> members;
};

// A control message's kind, its first byte. Saves, the creator's backup memory in its save file's form, comes just
// before Start.
enum class LockstepKind : uint8_t { Start = 'S', Saves = 'B', Lost = 'L', Hash = 'H', Ping = 'I', Pong = 'O', Late = 'X' };

std::vector<uint8_t> lockstep_start_message(const LockstepStart& s);
std::optional<LockstepStart> lockstep_read_start(const std::vector<uint8_t>& m);
std::vector<uint8_t> lockstep_time_message(LockstepKind kind, uint64_t ns);       // a ping, or the pong that echoes it
std::optional<uint64_t> lockstep_read_time(const std::vector<uint8_t>& m);
// The input delay for a round trip of `rtt_ns`: enough frames for the pads to arrive with a frame to spare, 2 to 8.
// With rollback, half that, at least 1.
int lockstep_delay_for(uint64_t rtt_ns, bool rollback = false);
// A hash of work RAM, for comparing peers' machines.
uint64_t lockstep_hash(const uint8_t* p, size_t n, uint64_t seed = 0);

class Lockstep {
public:
    Lockstep(const LockstepStart& start, int self);

    int self() const { return self_; }
    int delay() const { return delay_; }
    int ahead() const { return ahead_; }
    const std::vector<LockstepMember>& members() const { return members_; }
    int member_of(const std::string& id) const;   // -1: not in the game

    // This peer's own pads, sampled at VBlank `frame`, one per slot it owns. They apply at frame + delay.
    void local(uint64_t frame, const uint16_t* pads);
    // The pads message for `member`, with every frame of this peer's it has not acknowledged, oldest first.
    std::vector<uint8_t> pads_message(int member) const;
    bool hear_pads(int member, const std::vector<uint8_t>& m);   // false: not a pads message, and ignored

    bool ready(uint64_t frame) const;                     // every member's pads for `frame` are here
    uint64_t confirmed() const;                           // the first frame that is not ready
    std::vector<int> waiting_on(uint64_t frame) const;    // the members `frame` still needs pads from
    // A frame's pads, every slot: a missing one is its member's last.
    void pads(uint64_t frame, uint16_t out[kLockstepSlots]) const;
    bool owned(int slot) const;                           // a member, gone or not, owns the slot

    // `member` has gone. The report to send every other member still here, saying what this peer holds of it.
    std::vector<uint8_t> lose(int member);
    // A report from `from` that a member has gone. `self`: the report names this peer, which the rest have lost.
    // `report`: this peer had not lost that member yet, and sends its own report on it to the rest.
    struct LostHeard {
        bool self = false;
        std::vector<uint8_t> report;
    };
    LostHeard hear_lost(int from, const std::vector<uint8_t>& m);
    bool gone(int member) const { return members_state_[member].gone_from.has_value(); }
    std::optional<uint64_t> gone_from(int member) const { return members_state_[member].gone_from; }

    // Hashes of the machine at VBlank `frame`: this peer's, and another member's as it arrives. A frame whose two
    // differ is the frame the machines parted, which `parted` gives once it is known.
    std::vector<uint8_t> hash_message(uint64_t frame, uint64_t hash);
    void hear_hash(int from, const std::vector<uint8_t>& m);
    std::optional<uint64_t> parted() const { return parted_; }
    int parted_with() const { return parted_with_; }

    // The pads the machine ran `frame` with. The frames after it have not run.
    void ran(uint64_t frame, const uint16_t pads[kLockstepSlots]);
    // The first frame that ran with pads other than those it has now. Frames before that and before `confirmed`
    // can never be wrong, so they are forgotten.
    std::optional<uint64_t> first_wrong();
    // How many frames this peer is ahead of the member furthest behind it, from their pads messages.
    int lead() const;

    // Frames before `frame` that no member needs again.
    void forget_before(uint64_t frame);

private:
    struct Member {
        uint64_t base = 0;                        // the frame inputs[0] is for
        std::deque<std::vector<uint16_t>> inputs; // one entry a frame, `count` pads each, with no gaps
        uint64_t acked = 0;                       // this peer's frames the member holds, through this one (+1)
        std::optional<uint64_t> gone_from;        // its pads are idle from this frame
        bool leaving = false;                     // agreeing on gone_from
        std::vector<int> reports_due;             // the members whose report on it has not come
        uint64_t latest = 0;                      // the latest frame any report holds (+1)
        std::vector<std::pair<uint64_t, std::vector<uint16_t>>> reported;
        bool heard = false;                       // a pads message has come
        uint64_t at = 0;                          // the latest VBlank it said it was at
        int gap = 0;                              // how far ahead of it it said this peer was, then
    };

    uint64_t through(int member) const;           // the frame after the last one held of that member
    const std::vector<uint16_t>* input(int member, uint64_t frame) const;
    void store(int member, uint64_t frame, std::vector<uint16_t> pads);
    void settle(int member);

    std::vector<LockstepMember> members_;
    std::vector<Member> members_state_;
    int self_;
    int delay_;
    int ahead_;
    uint64_t start_;
    uint64_t now_;                                // the latest frame of this peer's own pads
    uint64_t ran_base_ = 0;                       // the frame ran_[0] is for
    std::deque<std::array<uint16_t, kLockstepSlots>> ran_;
    std::vector<std::pair<uint64_t, uint64_t>> own_hashes_;            // frame, hash
    std::vector<std::pair<int, std::pair<uint64_t, uint64_t>>> heard_hashes_;
    std::optional<uint64_t> parted_;
    int parted_with_ = -1;
};

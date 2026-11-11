// Pads in lockstep: what each member runs a frame with, what crosses between members, and how the rest agree when one
// goes.
#include "lockstep.h"
#include "test.h"
#include <vector>

namespace {

// Ann owns slot 0, Bob slots 1 and 2, Cat slot 3; the game starts at VBlank 100 with a delay of 2.
LockstepStart start(int members = 3) {
    LockstepStart s{.frame = 100, .delay = 2, .members = {{"ann", 0, 1}, {"bob", 1, 2}, {"cat", 3, 1}}};
    s.members.resize(members);
    return s;
}

// Each member's lockstep, and a way to pass every member's pads to every other.
struct Room {
    std::vector<Lockstep> peers;
    explicit Room(int members = 3) {
        for (int i = 0; i < members; ++i) peers.emplace_back(start(members), i);
    }
    void press(uint64_t frame, int member, uint16_t pad) {
        const std::vector<uint16_t> pads(peers[member].members()[member].count, pad);
        peers[member].local(frame, pads.data());
    }
    void exchange(const std::vector<int>& from = {0, 1, 2}) {
        for (int a : from)
            for (int b = 0; b < (int)peers.size(); ++b)
                if (a != b && a < (int)peers.size()) peers[b].hear_pads(a, peers[a].pads_message(b));
    }
};

uint16_t pad_at(const Lockstep& l, uint64_t frame, int slot) {
    uint16_t out[kLockstepSlots];
    l.pads(frame, out);
    return out[slot];
}

}  // namespace

TEST(lockstep_frames_before_the_delay_are_idle_and_ready) {
    Lockstep ann(start(), 0);
    CHECK_EQ(ann.ready(100), true);
    CHECK_EQ(ann.ready(101), true);
    CHECK_EQ(ann.ready(102), false);
    CHECK_EQ(pad_at(ann, 101, 0), 0);
}

TEST(lockstep_a_press_applies_after_the_delay_on_every_member) {
    Room room;
    room.press(100, 0, 0x0400);
    room.press(100, 1, 0x0800);
    room.press(100, 2, 0x8000);
    room.exchange();
    for (const Lockstep& l : room.peers) {
        CHECK_EQ(l.ready(102), true);
        CHECK_EQ(pad_at(l, 102, 0), 0x0400);
        CHECK_EQ(pad_at(l, 102, 1), 0x0800);
        CHECK_EQ(pad_at(l, 102, 2), 0x0800);
        CHECK_EQ(pad_at(l, 102, 3), 0x8000);
        CHECK_EQ(pad_at(l, 102, 4), 0);
        CHECK_EQ(l.ready(103), false);
    }
}

TEST(lockstep_a_frame_waits_for_the_member_whose_pads_have_not_come) {
    Room room;
    for (int m = 0; m < 3; ++m) room.press(100, m, 1);
    room.exchange({0, 1});
    CHECK_EQ(room.peers[0].ready(102), false);
    CHECK_EQ(room.peers[0].waiting_on(102).size(), 1);
    CHECK_EQ(room.peers[0].waiting_on(102)[0], 2);
}

// The pads channel never sends a message again, so the next message carries every frame not yet acknowledged.
TEST(lockstep_a_lost_message_is_made_good_by_the_next) {
    Room room(2);
    room.press(100, 0, 0x10);
    (void)room.peers[0].pads_message(1);   // lost
    room.press(101, 0, 0x20);
    room.peers[1].hear_pads(0, room.peers[0].pads_message(1));
    CHECK_EQ(pad_at(room.peers[1], 102, 0), 0x10);
    CHECK_EQ(pad_at(room.peers[1], 103, 0), 0x20);
}

TEST(lockstep_an_acknowledged_frame_is_not_sent_again) {
    Room room(2);
    room.press(100, 0, 0x10);
    room.peers[1].hear_pads(0, room.peers[0].pads_message(1));
    room.peers[0].hear_pads(1, room.peers[1].pads_message(0));   // Bob's message acknowledges Ann's frame 102
    room.press(101, 0, 0x20);
    const std::vector<uint8_t> m = room.peers[0].pads_message(1);
    CHECK_EQ(m[5] | m[6] << 8, 103);   // the first frame it holds
    CHECK_EQ(m[9], 1);                 // and only that one
}

TEST(lockstep_a_message_from_after_a_gap_waits_for_the_gap) {
    Room room(2);
    room.press(100, 0, 0x10);
    room.press(101, 0, 0x20);
    std::vector<uint8_t> late = room.peers[0].pads_message(1);
    late[5] = 103;   // claims to start at 103, past the 102 Bob lacks
    room.peers[1].hear_pads(0, late);
    CHECK_EQ(room.peers[1].ready(102), false);
}

TEST(lockstep_a_short_or_foreign_message_is_ignored) {
    Room room(2);
    room.press(100, 0, 0x10);
    std::vector<uint8_t> m = room.peers[0].pads_message(1);
    m.pop_back();
    CHECK_EQ(room.peers[1].hear_pads(0, m), false);
    CHECK_EQ(room.peers[1].hear_pads(0, {'Q', 1, 2}), false);
    CHECK_EQ(room.peers[1].ready(102), false);
}

TEST(lockstep_the_start_message_reads_back) {
    const std::vector<uint8_t> m = lockstep_start_message(start());
    const auto s = lockstep_read_start(m);
    CHECK_EQ(s.has_value(), true);
    CHECK_EQ(s->frame, 100);
    CHECK_EQ(s->delay, 2);
    CHECK_EQ(s->members.size(), 3);
    CHECK_EQ(s->members[1].id == "bob", true);
    CHECK_EQ(s->members[1].first, 1);
    CHECK_EQ(s->members[1].count, 2);
    std::vector<uint8_t> past = m;
    past[past.size() - 2] = 12;   // Cat's first slot, past the last
    CHECK_EQ(lockstep_read_start(past).has_value(), false);
}

// A watcher owns no slots, but each frame still waits for its message, which is how the others learn it is there.
TEST(lockstep_a_watcher_with_no_pads_reads_back_and_plays_in_step) {
    LockstepStart s{.frame = 100, .delay = 2, .members = {{"ann", 0, 1}, {"dee", 1, 0}}};
    const auto read = lockstep_read_start(lockstep_start_message(s));
    CHECK_EQ(read.has_value(), true);
    CHECK_EQ(read->members[1].count, 0);
    Lockstep ann(s, 0), dee(s, 1);
    const uint16_t press = 0x0400;
    ann.local(100, &press);
    dee.local(100, nullptr);
    CHECK_EQ(ann.ready(102), false);
    ann.hear_pads(1, dee.pads_message(0));
    dee.hear_pads(0, ann.pads_message(1));
    CHECK_EQ(ann.ready(102), true);
    CHECK_EQ(dee.ready(102), true);
    CHECK_EQ(pad_at(dee, 102, 0), press);
    CHECK_EQ(dee.owned(1), false);
}

// Bob holds Cat's pads through frame 104 and Ann through 102. Both settle on Cat's pads going idle at 105, with Ann
// given 103 and 104 from Bob's report, since Bob may already have run them.
TEST(lockstep_the_rest_agree_on_when_a_lost_member_goes_idle) {
    Room room;
    for (uint64_t f = 100; f < 103; ++f)
        for (int m = 0; m < 3; ++m) room.press(f, m, (uint16_t)(f << 4 | m));
    room.exchange({0, 1});
    room.peers[1].hear_pads(2, room.peers[2].pads_message(1));
    Lockstep& ann = room.peers[0];
    Lockstep& bob = room.peers[1];
    CHECK_EQ(ann.ready(102), false);
    CHECK_EQ(bob.ready(104), true);

    const std::vector<uint8_t> from_ann = ann.lose(2);
    CHECK_EQ(ann.gone(2), false);   // still owed Bob's report
    const Lockstep::LostHeard heard = bob.hear_lost(0, from_ann);
    CHECK_EQ(heard.self, false);
    CHECK_EQ(heard.report.empty(), false);   // Bob had not lost Cat, so it reports too
    CHECK_EQ(bob.gone_from(2).value_or(0), 105);
    ann.hear_lost(1, heard.report);
    CHECK_EQ(ann.gone_from(2).value_or(0), 105);
    for (uint64_t f = 102; f < 107; ++f) {
        CHECK_EQ(ann.ready(f), bob.ready(f));
        CHECK_EQ(pad_at(ann, f, 3), pad_at(bob, f, 3));
    }
    CHECK_EQ(pad_at(ann, 104, 3), 102 << 4 | 2);
    CHECK_EQ(pad_at(ann, 105, 3), 0);
}

TEST(lockstep_pads_from_a_member_once_reported_lost_change_nothing) {
    Room room(2);
    room.press(100, 1, 0x10);
    room.peers[0].lose(1);
    room.peers[0].hear_pads(1, room.peers[1].pads_message(0));
    CHECK_EQ(room.peers[0].gone_from(1).value_or(0), 102);
    CHECK_EQ(pad_at(room.peers[0], 102, 1), 0);
}

TEST(lockstep_a_member_the_rest_lost_hears_so) {
    Room room(2);
    CHECK_EQ(room.peers[1].hear_lost(0, room.peers[0].lose(1)).self, true);
}

// A third member going while the rest agree on a second: its report is no longer waited for.
TEST(lockstep_a_loss_during_another_still_settles) {
    Room room;
    room.peers[0].lose(2);
    CHECK_EQ(room.peers[0].gone(2), false);
    room.peers[0].lose(1);
    CHECK_EQ(room.peers[0].gone(2), true);
    CHECK_EQ(room.peers[0].gone(1), true);
}

TEST(lockstep_differing_hashes_name_the_frame_and_the_member) {
    Lockstep ann(start(), 0);
    Lockstep bob(start(), 1);
    ann.hear_hash(1, bob.hash_message(160, 7));
    CHECK_EQ(ann.parted().has_value(), false);
    ann.hash_message(160, 7);
    CHECK_EQ(ann.parted().has_value(), false);
    bob.hash_message(220, 8);
    ann.hash_message(220, 9);
    ann.hear_hash(1, bob.hash_message(220, 8));
    CHECK_EQ(ann.parted().value_or(0), 220);
    CHECK_EQ(ann.parted_with(), 1);
}

TEST(lockstep_the_delay_covers_half_the_round_trip_with_a_frame_to_spare) {
    CHECK_EQ(lockstep_delay_for(0), 2);
    CHECK_EQ(lockstep_delay_for(60'000'000), 4);
    CHECK_EQ(lockstep_delay_for(120'000'000), 6);
    CHECK_EQ(lockstep_delay_for(2'000'000'000), 8);
}

TEST(lockstep_forgotten_frames_leave_the_present_alone) {
    Room room(2);
    for (uint64_t f = 100; f < 300; ++f) {
        room.press(f, 0, (uint16_t)f);
        room.press(f, 1, (uint16_t)f);
        room.exchange({0, 1});
        room.peers[0].forget_before(f > 128 ? f - 128 : 0);
    }
    CHECK_EQ(room.peers[0].ready(301), true);
    CHECK_EQ(pad_at(room.peers[0], 301, 1), 299);
}

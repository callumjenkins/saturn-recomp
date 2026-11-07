// A run in a playtest room: what it knows of the room, the card it shows, the offer of another match, and the
// entries of OUT/line.txt.
#include "line_log.h"
#include "online_room.h"
#include "test.h"
#include <string>

namespace {

using Kind = SessionEvent::Kind;

SessionEvent event(Kind kind, std::string peer = "", std::string text = "") {
    return {.kind = kind, .peer = std::move(peer), .name = kind == Kind::Joined ? text : "", .text = text};
}

// In room BCDF with Ann, whose channels are open.
OnlineRoom with_ann() {
    OnlineRoom r;
    r.hear(event(Kind::Welcome, "", "BCDF"));
    r.hear(event(Kind::Joined, "p1", "Ann"));
    r.hear(event(Kind::Open, "p1"));
    return r;
}

bool card_is(const OnlineRoom& r, const std::string& text, bool again = false) {
    const OnlineCard c = r.card();
    return c.text == text && c.again == again;
}

}  // namespace

TEST(the_card_follows_the_room_from_joining_to_the_far_peer_leaving) {
    OnlineRoom r;
    CHECK_EQ(card_is(r, "Joining the room…"), true);
    r.hear(event(Kind::Welcome, "", "BCDF"));
    CHECK_EQ(card_is(r, "Room BCDF · Give the other player this code"), true);
    r.hear(event(Kind::Joined, "p1", "Ann"));
    CHECK_EQ(card_is(r, "Room BCDF · Connecting to Ann…"), true);
    CHECK_EQ(r.hear(event(Kind::Open, "p1")) == OnlineRoom::Far::Came, true);
    CHECK_EQ(r.far() == "p1", true);
    CHECK_EQ(card_is(r, "Room BCDF · With Ann"), true);
    r.called();
    CHECK_EQ(card_is(r, ""), true);
    CHECK_EQ(r.hear(event(Kind::Left, "p1", "left the room")) == OnlineRoom::Far::Went, true);
    CHECK_EQ(r.far().empty(), true);
    CHECK_EQ(card_is(r, "Room BCDF · Ann has left"), true);
}

TEST(a_second_peer_whose_channels_open_is_not_the_far_peer) {
    OnlineRoom r = with_ann();
    r.hear(event(Kind::Joined, "p2", "Bo"));
    CHECK_EQ(r.hear(event(Kind::Open, "p2")) == OnlineRoom::Far::Unchanged, true);
    CHECK_EQ(r.far() == "p1", true);
    CHECK_EQ(r.hear(event(Kind::Left, "p2", "left the room")) == OnlineRoom::Far::Unchanged, true);
    CHECK_EQ(card_is(r, "Room BCDF · With Ann"), true);
}

TEST(a_room_that_closes_keeps_the_far_peer_whose_connection_goes_on) {
    OnlineRoom r = with_ann();
    CHECK_EQ(r.hear(event(Kind::RoomClosed, "", "the room's socket closed")) == OnlineRoom::Far::Unchanged, true);
    CHECK_EQ(r.far() == "p1", true);
    CHECK_EQ(card_is(r, "The room closed: the room's socket closed"), true);
    CHECK_EQ(r.hear(event(Kind::Left, "p1", "left the room")) == OnlineRoom::Far::Went, true);
    OnlineRoom refused;
    refused.hear(event(Kind::Refused, "", "the game differs"));
    CHECK_EQ(card_is(refused, "The room refused this game: the game differs"), true);
}

TEST(another_match_is_offered_from_the_programs_second_start_while_the_far_peer_is_there) {
    OnlineRoom alone;
    alone.hear(event(Kind::Welcome, "", "BCDF"));
    CHECK_EQ(alone.program_started(100), false);
    CHECK_EQ(alone.program_started(200), false);
    OnlineRoom r = with_ann();
    r.called();
    CHECK_EQ(r.program_started(100), false);
    CHECK_EQ(r.program_started(300), true);
    CHECK_EQ(card_is(r, "Room BCDF · Start: play Ann again", true), true);
    CHECK_EQ(r.take_offer().value_or(0), 300);
    CHECK_EQ(r.take_offer().has_value(), false);
    CHECK_EQ(card_is(r, "Room BCDF · With Ann"), true);
    CHECK_EQ(r.program_started(400), true);
    r.hear(event(Kind::Left, "p1", "left the room"));
    CHECK_EQ(r.take_offer().has_value(), false);
}

TEST(a_taken_offer_counts_its_script_from_the_offer_or_late_enough_that_its_first_press_comes_now) {
    CHECK_EQ(online_again_from(1000, 50, 1020), 1000);
    CHECK_EQ(online_again_from(1000, 50, 1050), 1000);
    CHECK_EQ(online_again_from(1000, 50, 1100), 1050);
}

TEST(a_taken_offer_presses_the_saved_script_only_when_the_save_file_held_its_save) {
    auto held = [](const std::string& name) { return name == "XBAND____01"; };
    CHECK_EQ(online_again_script("741:A", "XBAND____01:2700:A", held) == "2700:A", true);
    CHECK_EQ(online_again_script("741:A", "OTHER:2700:A", held) == "741:A", true);
    CHECK_EQ(online_again_script("741:A", "", held) == "741:A", true);
}

TEST(a_line_log_entry_reads_back_as_what_the_line_heard) {
    for (const LineHeard& h : {LineHeard{12, true, {}}, LineHeard{13, false, {}}, LineHeard{14, true, {0x44, 0x00, 0xFF}}}) {
        const std::optional<LineHeard> back = line_log_read(line_log_entry(h));
        CHECK_EQ(back.has_value(), true);
        CHECK_EQ(back->at, h.at);
        CHECK_EQ(back->reachable, h.reachable);
        CHECK_EQ(back->message == h.message, true);
    }
    CHECK_EQ(line_log_entry({14, true, {0x44, 0x00, 0xFF}}) == "14 heard 4400FF", true);
    CHECK_EQ(line_log_entry({13, false, {}}) == "13 reachable 0", true);
}

TEST(a_line_log_entry_that_does_not_read_is_refused) {
    for (const char* bad : {"", "12", "x reachable 1", "12 reachable", "12 reachable 2", "12 reachable 1 more",
                            "12 heard", "12 heard 4", "12 heard 4G", "12 sent 44"})
        CHECK_EQ(line_log_read(bad).has_value(), false);
}

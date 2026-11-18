// saturn-recomp runtime — what a run in a playtest room knows of it (online_room.h).
#include "online_room.h"

OnlineRoom::Far OnlineRoom::hear(const SessionEvent& e) {
    switch (e.kind) {
    case SessionEvent::Kind::Welcome: code_ = e.text; break;
    case SessionEvent::Kind::Joined: names_[e.peer] = e.name; break;
    case SessionEvent::Kind::Open:
        if (!far_.empty()) break;
        far_ = e.peer;
        gone_.clear();
        called_ = false;
        return Far::Came;
    case SessionEvent::Kind::Message: break;
    case SessionEvent::Kind::Left: {
        const bool was_far = e.peer == far_;
        if (was_far) gone_ = name(far_);
        names_.erase(e.peer);
        if (!was_far) break;
        far_.clear();
        offered_ = false;
        return Far::Went;
    }
    case SessionEvent::Kind::Refused: trouble_ = "The room refused this game: " + e.text; break;
    case SessionEvent::Kind::RoomClosed: trouble_ = "The room closed: " + e.text; break;
    }
    return Far::Unchanged;
}

bool OnlineRoom::program_started(uint64_t now) {
    if (++program_starts_ < 2 || far_.empty()) return false;
    offered_ = true;
    offered_at_ = now;
    return true;
}

std::optional<uint64_t> OnlineRoom::take_offer() {
    if (!offered_) return std::nullopt;
    offered_ = false;
    called_ = false;
    return offered_at_;
}

OnlineCard OnlineRoom::card() const {
    if (!trouble_.empty()) return {trouble_};
    if (code_.empty()) return {"Joining the room…"};
    const std::string room = "Room " + code_ + " · ";
    if (far_.empty() && !names_.empty()) return {room + "Connecting to " + names_.begin()->second + "…"};
    if (!gone_.empty()) return {room + gone_ + " has left"};
    if (far_.empty()) return {room + "Give the other player this code"};
    if (offered_) return {room + "Start: play " + name(far_) + " again", true};
    if (!called_) return {room + "With " + name(far_)};
    return {};
}

std::string OnlineRoom::name(const std::string& peer) const {
    const auto it = names_.find(peer);
    return it == names_.end() ? "" : it->second;
}

uint64_t online_again_from(uint64_t offered_at, uint64_t first_press, uint64_t now) {
    return offered_at + first_press < now ? now - first_press : offered_at;
}

std::string online_again_script(const std::string& script, const std::string& saved,
                                const std::function<bool(const std::string&)>& held) {
    const size_t colon = saved.find(':');
    if (colon == std::string::npos || !held(saved.substr(0, colon))) return script;
    return saved.substr(colon + 1);
}

// saturn-recomp runtime — which player each controller drives (host.cpp).
//
// Controllers take the lowest free slot as they connect, so the first plays
// as player 1, beside the keyboard. One that disconnects frees its slot and
// moves nobody else. A controller connecting gets back a slot its model left
// only when no other slot was left by the same model: identical pads report
// the same GUID and no serial number, so that is the one case where the slot
// it left is certain.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class PadSlots {
public:
    explicit PadSlots(int slots) : held_(slots, kFree), left_(slots), guids_(slots) {}

    // The slot the controller now drives, or -1 when every slot is taken.
    int connect(uint32_t id, const std::string& guid) {
        int back = -1, matches = 0;
        for (int s = 0; s < (int)held_.size(); ++s)
            if (held_[s] == kFree && !left_[s].empty() && left_[s] == guid) { back = s; ++matches; }
        int s = matches == 1 ? back : lowest_free();
        if (s < 0) return -1;
        held_[s] = id;
        left_[s].clear();
        guids_[s] = guid;
        return s;
    }

    // The slot it drove, now free, or -1 for a controller that had none.
    int disconnect(uint32_t id) {
        int s = slot_of(id);
        if (s < 0) return -1;
        left_[s] = guids_[s];
        held_[s] = kFree;
        return s;
    }

    int slot_of(uint32_t id) const {
        for (int s = 0; s < (int)held_.size(); ++s)
            if (held_[s] == id) return s;
        return -1;
    }

    bool taken(int slot) const { return held_[slot] != kFree; }

private:
    static constexpr uint32_t kFree = 0;   // SDL never gives a device ID of 0

    int lowest_free() const {
        for (int s = 0; s < (int)held_.size(); ++s)
            if (held_[s] == kFree) return s;
        return -1;
    }

    std::vector<uint32_t> held_;        // the controller in each slot
    std::vector<std::string> left_;     // the GUID of the controller that last left each free slot
    std::vector<std::string> guids_;    // the GUID of the controller in each slot
};

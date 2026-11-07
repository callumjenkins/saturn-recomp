// saturn-recomp runtime — OUT/line.txt: what a session's modem line heard, a line each, which --line-in plays
// to the line again in a replay.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What the line heard, at the machine time of the poll that handed it over: the far peer coming or going, or a message.
struct LineHeard {
    uint64_t at = 0;
    bool reachable = false;
    std::vector<uint8_t> message;   // empty: `reachable` changed
};

// "TIME reachable 0|1" or "TIME heard HEX", without the newline.
std::string line_log_entry(const LineHeard& h);
std::optional<LineHeard> line_log_read(const std::string& entry);

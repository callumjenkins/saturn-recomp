// saturn-recomp runtime — OUT/line.txt's entries (line_log.h).
#include "line_log.h"
#include <cctype>
#include <cstdio>
#include <sstream>

std::string line_log_entry(const LineHeard& h) {
    std::string out = std::to_string(h.at);
    if (h.message.empty()) return out + (h.reachable ? " reachable 1" : " reachable 0");
    out += " heard ";
    char hex[3];
    for (uint8_t b : h.message) {
        std::snprintf(hex, sizeof hex, "%02X", b);
        out += hex;
    }
    return out;
}

std::optional<LineHeard> line_log_read(const std::string& entry) {
    std::istringstream fields(entry);
    LineHeard h;
    std::string what, value, rest;
    if (!(fields >> h.at >> what >> value) || fields >> rest) return std::nullopt;
    if (what == "reachable") {
        if (value != "0" && value != "1") return std::nullopt;
        h.reachable = value == "1";
        return h;
    }
    if (what != "heard" || value.size() % 2) return std::nullopt;
    for (char c : value)
        if (!std::isxdigit((unsigned char)c)) return std::nullopt;
    h.reachable = true;
    for (size_t i = 0; i < value.size(); i += 2) h.message.push_back((uint8_t)std::stoul(value.substr(i, 2), nullptr, 16));
    return h;
}

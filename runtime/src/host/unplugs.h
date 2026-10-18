// saturn-recomp runtime — --virtual-unplug's list of when the virtual gamepads are unplugged (host.cpp).
#pragma once
#include "host.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

struct PlugStep { uint64_t vblank; int pad; bool plugged; };   // pad from 0

// "PAD:FROM-TO,...": pad PAD, from 1, unplugged at VBlank FROM and plugged back in at TO, as steps in
// VBlank order. False when the spec does not read, a pad is 0 or past the last player, TO is not after
// FROM, or one pad's times overlap.
inline bool parse_unplugs(const std::string& spec, std::vector<PlugStep>& out) {
    out.clear();
    if (!spec.empty() && spec.back() == ',') return false;
    size_t at = 0;
    while (at < spec.size()) {
        size_t end = spec.find(',', at);
        if (end == std::string::npos) end = spec.size();
        std::string item = spec.substr(at, end - at);
        at = end + 1;
        char* p = item.data();
        auto number = [&](unsigned long long& n) {
            if (*p < '0' || *p > '9') return false;
            n = std::strtoull(p, &p, 10);
            return true;
        };
        unsigned long long pad, from, to;
        if (!number(pad) || pad < 1 || pad > kHostSlots || *p++ != ':' || !number(from) || *p++ != '-' || !number(to) || *p || to <= from)
            return false;
        out.push_back({from, (int)pad - 1, false});
        out.push_back({to, (int)pad - 1, true});
    }
    // on one VBlank, a pad comes back before any goes
    std::stable_sort(out.begin(), out.end(), [](const PlugStep& a, const PlugStep& b) {
        return a.vblank != b.vblank ? a.vblank < b.vblank : a.plugged > b.plugged;
    });
    bool unplugged[kHostSlots] = {};
    for (const PlugStep& s : out) {
        if (unplugged[s.pad] == !s.plugged) return false;   // unplugged twice, or back while it was in
        unplugged[s.pad] = !s.plugged;
    }
    return true;
}

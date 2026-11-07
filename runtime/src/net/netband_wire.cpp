// saturn-recomp runtime — the NetBand tunnel's wire format (netband_wire.h).
#include "netband_wire.h"
#include <algorithm>
#include <cstdlib>
#include <regex>
#include <string_view>

namespace {

const std::string_view kPacketSplit = "<packetSplit>";
const std::string_view kDataSplit = "<dataSplit>";
const size_t kKept = 5;                     // payloads a datagram holds
const uint32_t kStunCookie = 0x2112A442;
const char* const kMatchKey = "mySuperSecretSaturnKey1234";   // the one every tunnel sends, published in its source

std::vector<std::vector<uint8_t>> split(const std::vector<uint8_t>& data, std::string_view by) {
    std::vector<std::vector<uint8_t>> parts;
    std::string_view rest(reinterpret_cast<const char*>(data.data()), data.size());
    for (;;) {
        size_t at = rest.find(by);
        std::string_view part = rest.substr(0, at);
        parts.emplace_back(part.begin(), part.end());
        if (at == std::string_view::npos) return parts;
        rest.remove_prefix(at + by.size());
    }
}

// A framed payload's bytes and sequence number; empty when its number does not read.
std::optional<std::pair<std::vector<uint8_t>, uint64_t>> unframe(const std::vector<uint8_t>& framed) {
    std::vector<std::vector<uint8_t>> parts = split(framed, kDataSplit);
    if (parts.size() < 2 || parts[1].empty() || parts[1].size() > 19) return std::nullopt;
    uint64_t sequence = 0;
    for (uint8_t c : parts[1]) {
        if (c < '0' || c > '9') return std::nullopt;
        sequence = sequence * 10 + (c - '0');
    }
    return std::make_pair(std::move(parts[0]), sequence);
}

uint16_t be16(const std::vector<uint8_t>& d, size_t at) { return uint16_t(d[at] << 8 | d[at + 1]); }
uint32_t be32(const std::vector<uint8_t>& d, size_t at) { return uint32_t(be16(d, at)) << 16 | be16(d, at + 2); }

}  // namespace

std::vector<uint8_t> NetBandSender::datagram(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> framed = payload;
    framed.insert(framed.end(), kDataSplit.begin(), kDataSplit.end());
    std::string number = std::to_string(sequence_++);
    framed.insert(framed.end(), number.begin(), number.end());
    recent_.push_front(std::move(framed));
    if (recent_.size() > kKept) recent_.pop_back();
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& p : recent_) {
        if (!out.empty()) out.insert(out.end(), kPacketSplit.begin(), kPacketSplit.end());
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

std::vector<uint8_t> NetBandReceiver::take(const std::vector<uint8_t>& datagram) {
    std::vector<std::pair<std::vector<uint8_t>, uint64_t>> packets;
    for (const std::vector<uint8_t>& framed : split(datagram, kPacketSplit)) {
        auto p = unframe(framed);
        if (!p) return {};   // the tunnel drops a datagram it cannot read
        packets.push_back(std::move(*p));
    }
    std::vector<uint8_t> out;
    for (;;) {
        size_t at = 0;
        while (at < packets.size() && packets[at].second != next_) ++at;
        if (at == packets.size()) at = 0;
        if (packets[at].second < next_) return out;
        next_ = packets[at].second + 1;
        out.insert(out.end(), packets[at].first.begin(), packets[at].first.end());
        if (at == 0) return out;
    }
}

NetBandSignal netband_signal(const std::vector<uint8_t>& datagram) {
    std::string_view d(reinterpret_cast<const char*>(datagram.data()), datagram.size());
    if (d == "PING_SHIRO") return NetBandSignal::Ping;
    if (d == "PONG_SHIRO") return NetBandSignal::Pong;
    if (d == "OPEN_SHIRO") return NetBandSignal::Open;
    return NetBandSignal::None;
}

std::vector<uint8_t> netband_signal_datagram(NetBandSignal s) {
    std::string_view text = s == NetBandSignal::Ping ? "PING_SHIRO" : s == NetBandSignal::Pong ? "PONG_SHIRO" : "OPEN_SHIRO";
    return {text.begin(), text.end()};
}

std::string netband_ip12(const std::string& dotted) {
    std::string out;
    size_t at = 0;
    for (int part = 0; part < 4; ++part) {
        const size_t end = std::min(dotted.find('.', at), dotted.size());
        const std::string digits = dotted.substr(at, end - at);
        if (digits.empty() || digits.size() > 3 || digits.find_first_not_of("0123456789") != std::string::npos ||
            std::stoi(digits) > 255 || (part < 3) != (end < dotted.size()))
            return "";
        out += std::string(3 - digits.size(), '0') + digits;
        at = end + 1;
    }
    return out;
}

std::optional<std::string> netband_dotted(const std::string& ip12) {
    if (ip12.size() != 12 || ip12.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    std::string out;
    for (int i = 0; i < 4; ++i) {
        int part = std::stoi(ip12.substr(i * 3, 3));
        if (part > 255) return std::nullopt;
        out += (i ? "." : "") + std::to_string(part);
    }
    return out;
}

std::vector<uint8_t> stun_request(const std::array<uint8_t, 12>& id) {
    std::vector<uint8_t> m = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    m.insert(m.end(), id.begin(), id.end());
    return m;
}

bool stun_message(const std::vector<uint8_t>& datagram) {
    return datagram.size() >= 20 && (datagram[0] & 0xC0) == 0 && be32(datagram, 4) == kStunCookie;
}

std::optional<NetBandAddress> stun_mapped(const std::vector<uint8_t>& reply, const std::array<uint8_t, 12>& id) {
    if (!stun_message(reply) || be16(reply, 0) != 0x0101 || !std::equal(id.begin(), id.end(), reply.begin() + 8))
        return std::nullopt;
    size_t end = std::min(reply.size(), size_t(20) + be16(reply, 2));
    std::optional<NetBandAddress> plain;
    for (size_t at = 20; at + 4 <= end;) {
        uint16_t type = be16(reply, at), length = be16(reply, at + 2);
        size_t value = at + 4;
        if (value + length > end) break;
        const bool ipv4 = length >= 8 && reply[value + 1] == 0x01;
        if ((type == 0x0020 || type == 0x0001) && ipv4) {
            uint16_t port = be16(reply, value + 2);
            uint32_t ip = be32(reply, value + 4);
            if (type == 0x0020) {   // XOR-MAPPED-ADDRESS, the address and port XORed with the cookie
                port ^= kStunCookie >> 16;
                ip ^= kStunCookie;
            }
            NetBandAddress a{std::to_string(ip >> 24) + "." + std::to_string(ip >> 16 & 255) + "." +
                                 std::to_string(ip >> 8 & 255) + "." + std::to_string(ip & 255),
                             port};
            if (type == 0x0020) return a;
            plain = a;
        }
        at = value + ((length + 3) & ~size_t(3));
    }
    return plain;
}

std::string netband_query(const std::string& action, const std::string& code, const std::string& ip12, int port) {
    std::string q = "action=" + action + "&gameID=" + code.substr(code.size() < 2 ? 0 : code.size() - 2) +
                    "&client_ip=" + ip12;
    if (action == "wait" || action == "match") q += "&port=" + std::to_string(port);
    return q + "&key=" + kMatchKey;
}

std::optional<NetBandAddress> netband_opponent(const std::string& reply, const std::string& matched) {
    static const std::regex status(R"re("status"\s*:\s*"([^"]*)")re");
    static const std::regex far(R"re("opponent ip_address"\s*:\s*\[\s*"(\d{12})"\s*,\s*"?(\d{1,5})"?\s*\])re");
    std::smatch s, f;
    if (!std::regex_search(reply, s, status) || s[1] != matched || !std::regex_search(reply, f, far)) return std::nullopt;
    std::optional<std::string> ip = netband_dotted(f[1]);
    const int port = std::stoi(f[2]);
    if (!ip || port <= 0 || port > 65535) return std::nullopt;
    return NetBandAddress{*ip, port};
}

std::optional<NetBandSpec> netband_spec(const std::string& text, std::string& error) {
    const bool is_code = text.size() == 8 && text.find_first_not_of("0123456789", 5) == std::string::npos;
    NetBandSpec spec;
    if (text == "wait") {
        spec.wait = true;
    } else if (text.rfind("wait:", 0) == 0 && is_code) {
        spec.wait = true;
        spec.code = text.substr(5);
    } else if (text.rfind("dial:", 0) == 0 && is_code) {
        spec.code = text.substr(5);
    } else if (text.rfind("dial:", 0) == 0 && !netband_ip12(text.substr(5)).empty()) {
        spec.ip = text.substr(5);
    } else {
        error = "netband= takes wait, wait:CODE, dial:CODE, or dial:IP, CODE being the game's 3 digits";
        return std::nullopt;
    }
    return spec;
}

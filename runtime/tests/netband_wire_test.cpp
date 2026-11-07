// The NetBand tunnel's wire format, against the bytes the tunnel's own client sends and expects.
#include "netband_wire.h"
#include "test.h"
#include <string>

namespace {

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string text(const std::vector<uint8_t>& b) { return {b.begin(), b.end()}; }

const std::array<uint8_t, 12> kId = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

// A binding success reply to kId holding `attributes`.
std::vector<uint8_t> stun_reply(const std::vector<uint8_t>& attributes) {
    std::vector<uint8_t> r = {0x01, 0x01, 0, (uint8_t)attributes.size(), 0x21, 0x12, 0xA4, 0x42};
    r.insert(r.end(), kId.begin(), kId.end());
    r.insert(r.end(), attributes.begin(), attributes.end());
    return r;
}

}  // namespace

TEST(a_datagram_holds_the_newest_five_payloads_newest_first) {
    NetBandSender s;
    CHECK_EQ(text(s.datagram(bytes("ab"))) == "ab<dataSplit>0", true);
    CHECK_EQ(text(s.datagram(bytes("c"))) == "c<dataSplit>1<packetSplit>ab<dataSplit>0", true);
    for (const char* p : {"d", "e", "f"}) s.datagram(bytes(p));
    CHECK_EQ(text(s.datagram(bytes("g"))) ==
                 "g<dataSplit>5<packetSplit>f<dataSplit>4<packetSplit>e<dataSplit>3<packetSplit>d<dataSplit>2"
                 "<packetSplit>c<dataSplit>1",
             true);
}

TEST(a_receiver_takes_each_payload_once_and_in_order) {
    NetBandSender s;
    NetBandReceiver r;
    std::vector<uint8_t> first = s.datagram(bytes("ab"));
    CHECK_EQ(text(r.take(first)) == "ab", true);
    CHECK_EQ(r.take(first).empty(), true);   // the tunnel sends each datagram twice
    s.datagram(bytes("c"));                  // lost
    CHECK_EQ(text(r.take(s.datagram(bytes("d")))) == "cd", true);
}

TEST(a_receiver_that_lost_more_than_a_datagram_holds_skips_to_the_newest) {
    NetBandSender s;
    NetBandReceiver r;
    r.take(s.datagram(bytes("a")));
    for (const char* p : {"b", "c", "d", "e", "f"}) s.datagram(bytes(p));
    CHECK_EQ(text(r.take(s.datagram(bytes("g")))) == "g", true);
    CHECK_EQ(text(r.take(s.datagram(bytes("h")))) == "h", true);
}

TEST(a_datagram_that_does_not_read_delivers_nothing) {
    NetBandReceiver r;
    for (const char* bad : {"ab", "ab<dataSplit>", "ab<dataSplit>x1", "b<dataSplit>1<packetSplit>a",
                            "ab<dataSplit>12345678901234567890"})
        CHECK_EQ(r.take(bytes(bad)).empty(), true);
    CHECK_EQ(text(r.take(bytes("ok<dataSplit>0"))) == "ok", true);
}

TEST(pings_pongs_and_the_opening_datagram_are_signals) {
    CHECK_EQ(netband_signal(bytes("PING_SHIRO")) == NetBandSignal::Ping, true);
    CHECK_EQ(netband_signal(bytes("PONG_SHIRO")) == NetBandSignal::Pong, true);
    CHECK_EQ(netband_signal(bytes("OPEN_SHIRO")) == NetBandSignal::Open, true);
    CHECK_EQ(netband_signal(bytes("PING_SHIRO<dataSplit>0")) == NetBandSignal::None, true);
    CHECK_EQ(text(netband_signal_datagram(NetBandSignal::Pong)) == "PONG_SHIRO", true);
}

TEST(the_match_service_writes_an_address_as_twelve_digits) {
    CHECK_EQ(netband_ip12("192.168.1.2") == "192168001002", true);
    CHECK_EQ(netband_ip12("8.8.4.4") == "008008004004", true);
    CHECK_EQ(netband_ip12("256.1.1.1").empty(), true);
    for (const char* bad : {"1.2.3", "1.2.3.4x", "1.2.3.4.5", "+1.2.3.4", " 1.2.3.4", "1. 2.3.4", "1..3.4", "1.2.3.0004"})
        CHECK_EQ(netband_ip12(bad).empty(), true);
    CHECK_EQ(netband_dotted("192168001002").value_or("") == "192.168.1.2", true);
    CHECK_EQ(netband_dotted("19216800100").has_value(), false);
    CHECK_EQ(netband_dotted("300168001002").has_value(), false);
}

TEST(a_stun_reply_gives_the_mapped_address_xored_or_plain) {
    std::vector<uint8_t> request = stun_request(kId);
    CHECK_EQ(request.size(), 20);
    CHECK_EQ(request[0] << 8 | request[1], 0x0001);
    // 203.0.113.5:20001, XORed with the cookie: port 0x4E21 ^ 0x2112, address 0xCB007105 ^ 0x2112A442
    std::vector<uint8_t> xored = stun_reply({0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0x6F, 0x33, 0xEA, 0x12, 0xD5, 0x47});
    CHECK_EQ(stun_message(xored), true);
    CHECK_EQ(stun_mapped(xored, kId).value_or(NetBandAddress{}) == (NetBandAddress{"203.0.113.5", 20001}), true);
    std::vector<uint8_t> plain = stun_reply({0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x4E, 0x21, 203, 0, 113, 5});
    CHECK_EQ(stun_mapped(plain, kId).value_or(NetBandAddress{}) == (NetBandAddress{"203.0.113.5", 20001}), true);
    std::array<uint8_t, 12> other = kId;
    other[0] = 99;
    CHECK_EQ(stun_mapped(plain, other).has_value(), false);
    // Both kinds, plain first, as some servers send them: the XORed one is the one to trust.
    std::vector<uint8_t> both = stun_reply({0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x50, 10, 0, 0, 1,
                                            0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0x6F, 0x33, 0xEA, 0x12, 0xD5, 0x47});
    CHECK_EQ(stun_mapped(both, kId).value_or(NetBandAddress{}) == (NetBandAddress{"203.0.113.5", 20001}), true);
    // An IPv6 address first is passed over for the IPv4 one after it.
    std::vector<uint8_t> v6(24, 0);
    v6[1] = 0x20, v6[3] = 20, v6[5] = 0x02;
    v6.insert(v6.end(), {0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x4E, 0x21, 203, 0, 113, 5});
    CHECK_EQ(stun_mapped(stun_reply(v6), kId).value_or(NetBandAddress{}) == (NetBandAddress{"203.0.113.5", 20001}), true);
    CHECK_EQ(stun_message(bytes("ab<dataSplit>0 and some more bytes")), false);
}

TEST(a_match_query_names_the_game_by_its_codes_last_two_digits) {
    CHECK_EQ(netband_query("wait", "123", "192168001002", 20001) ==
                 "action=wait&gameID=23&client_ip=192168001002&port=20001&key=mySuperSecretSaturnKey1234",
             true);
    CHECK_EQ(netband_query("status", "123", "192168001002", 20001) ==
                 "action=status&gameID=23&client_ip=192168001002&key=mySuperSecretSaturnKey1234",
             true);
    CHECK_EQ(netband_query("match", "123", "192168001002", 20002) ==
                 "action=match&gameID=23&client_ip=192168001002&port=20002&key=mySuperSecretSaturnKey1234",
             true);
    CHECK_EQ(netband_query("timeout", "123", "192168001002", 20001) ==
                 "action=timeout&gameID=23&client_ip=192168001002&key=mySuperSecretSaturnKey1234",
             true);
}

TEST(a_match_reply_gives_the_opponent_only_with_the_status_asked_for) {
    const std::string matched = R"({"status": "matched", "opponent ip_address": ["203000113005", "20002"]})";
    CHECK_EQ(netband_opponent(matched, "matched").value_or(NetBandAddress{}) == (NetBandAddress{"203.0.113.5", 20002}), true);
    CHECK_EQ(netband_opponent(matched, "found opponent").has_value(), false);
    const std::string found = R"({"status":"found opponent","opponent ip_address":["203000113005",20001]})";
    CHECK_EQ(netband_opponent(found, "found opponent").value_or(NetBandAddress{}) == (NetBandAddress{"203.0.113.5", 20001}), true);
    CHECK_EQ(netband_opponent(R"({"status": "waiting"})", "matched").has_value(), false);
    CHECK_EQ(netband_opponent("not json", "matched").has_value(), false);
}

TEST(a_netband_spec_waits_or_dials_by_code_or_address) {
    std::string error;
    std::optional<NetBandSpec> s = netband_spec("wait", error);
    CHECK_EQ(s && s->wait && s->code.empty(), true);
    s = netband_spec("wait:123", error);
    CHECK_EQ(s && s->wait && s->code == "123", true);
    s = netband_spec("dial:123", error);
    CHECK_EQ(s && !s->wait && s->code == "123", true);
    s = netband_spec("dial:192.168.1.2", error);
    CHECK_EQ(s && !s->wait && s->ip == "192.168.1.2", true);
    for (const char* bad : {"", "dial", "wait:12", "dial:12a", "dial:1234", "dial:host.example", "call:123",
                            "dial: 1.2.3.4", "dial:+1.2.3.4"})
        CHECK_EQ(netband_spec(bad, error).has_value(), false);
}

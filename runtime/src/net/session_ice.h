// saturn-recomp runtime — the ICE servers a room's welcome lists (RTCIceServer's form), as libdatachannel takes them.
#pragma once
#include <nlohmann/json.hpp>
#include <rtc/configuration.hpp>
#include <vector>

// libjuice relays over UDP alone, so TURN over TCP or TLS is left out, and so is port 53, which networks often block.
std::vector<rtc::IceServer> session_ice_servers(const nlohmann::json& servers);

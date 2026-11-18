// saturn-recomp runtime — one HTTP GET, over TLS for an https:// URL, waiting for the whole reply.
#pragma once
#include <optional>
#include <string>

// The body of a 2xx reply to GET `url`, or nothing with the reason in `error`.
std::optional<std::string> http_get(const std::string& url, std::string& error, int timeout_ms = 10000);

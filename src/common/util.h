#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace squeeze2raop2 {

uint32_t hash32(std::string_view s);

// Trim ASCII whitespace from both ends of `s` (view, no allocation).
std::string_view trimView(std::string_view s);

// ASCII-lowercased copy of `s`.
std::string toLower(std::string_view s);

// Parses mDNS TXT wire format: a sequence of (len byte, len-1 data bytes),
// each data chunk split at its first '=' into key/value. First occurrence
// of a key wins.
std::map<std::string, std::string> parseTxtKeyValues(std::string_view raw);

std::string macToString(const std::array<uint8_t, 6>& mac);
bool macFromString(std::string_view s, std::array<uint8_t, 6>& out);

std::array<uint8_t, 6> fakeMacFor(std::string_view deviceId);

// Milliseconds per second, for ms<->frames/seconds conversions.
constexpr uint32_t kMsPerSecond = 1000;

uint64_t nowMs();

std::string urlDecode(std::string_view s);

// Thread-safe errno -> text (strerror_r/std::generic_category under the hood).
std::string errnoMessage(int err);

}  // namespace squeeze2raop2

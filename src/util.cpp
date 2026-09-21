#include "util.h"

#include <charconv>
#include <chrono>
#include <cctype>
#include <format>

namespace sq2 {

uint32_t hash32(std::string_view s) {
    uint32_t h = 2166136261u;
    for (char raw : s) {
        const auto c = static_cast<unsigned char>(raw);
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::string macToString(const std::array<uint8_t, 6>& mac) {
    return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2],
                       mac[3], mac[4], mac[5]);
}

bool macFromString(std::string_view s, std::array<uint8_t, 6>& out) {
    for (size_t i = 0; i < 6; ++i) {
        unsigned v = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
        if (ec != std::errc{} || v > 0xFF) return false;
        out[i] = static_cast<uint8_t>(v);
        if (i == 5) return ptr == s.data() + s.size();
        if (ptr == s.data() + s.size() || *ptr != ':') return false;
        s.remove_prefix(static_cast<size_t>(ptr + 1 - s.data()));
    }
    return false;
}

std::array<uint8_t, 6> fakeMacFor(std::string_view deviceId) {
    uint32_t h = hash32(deviceId);
    std::array<uint8_t, 6> mac{0xaa, 0x00, 0x00, 0x00, 0x00, 0x00};
    mac[1] = static_cast<uint8_t>((h >> 24) & 0xFF);
    mac[2] = static_cast<uint8_t>((h >> 16) & 0xFF);
    mac[3] = static_cast<uint8_t>((h >> 8) & 0xFF);
    mac[4] = static_cast<uint8_t>((h >> 0) & 0xFF);
    mac[5] = 0x01;
    return mac;
}

uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string urlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out.push_back(static_cast<char>((hex(s[i + 1]) << 4) | hex(s[i + 2])));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

}

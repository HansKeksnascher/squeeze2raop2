#include "util.h"

#include <chrono>
#include <cctype>

namespace sq2 {

uint32_t hash32(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::string macToString(const std::array<uint8_t, 6>& mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

bool macFromString(const std::string& s, std::array<uint8_t, 6>& out) {
    unsigned v[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (size_t i = 0; i < 6; ++i)
        if (v[i] > 0xFF) return false;
    for (size_t i = 0; i < 6; ++i)
        out[i] = static_cast<uint8_t>(v[i]);
    return true;
}

std::array<uint8_t, 6> fakeMacFor(const std::string& deviceId) {
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
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
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

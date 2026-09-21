#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace sq2 {

inline void packN(void* dst, uint64_t value, size_t bytes) {
    auto* out = static_cast<unsigned char*>(dst);
    for (size_t i = 0; i < bytes; ++i)
        out[i] = static_cast<unsigned char>((value >> ((bytes - 1 - i) * 8)) & 0xFF);
}

inline uint64_t unpackN(const void* src, size_t bytes) {
    auto* in = static_cast<const unsigned char*>(src);
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
        value = (value << 8) | in[i];
    return value;
}

uint32_t hash32(const std::string& s);

std::string macToString(const std::array<uint8_t, 6>& mac);
bool macFromString(const std::string& s, std::array<uint8_t, 6>& out);

std::array<uint8_t, 6> fakeMacFor(const std::string& deviceId);

uint64_t nowMs();

std::string urlDecode(const std::string& s);

}

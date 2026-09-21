#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace squeeze2raop2 {

// Big-endian byte packing: writes `bytes` octets of `value`, MSB first.
inline void packN(std::span<std::byte> dst, uint64_t value, size_t bytes) {
    bytes = std::min(bytes, dst.size());
    for (size_t i = 0; i < bytes; ++i)
        dst[i] = std::byte{static_cast<unsigned char>((value >> ((bytes - 1 - i) * 8)) & 0xFF)};
} // namespace squeeze2raop2

// Inverse of packN: reads all octets of `src`, MSB first.
inline uint64_t unpackN(std::span<const std::byte> src) {
    uint64_t value = 0;
    for (std::byte b : src)
        value = (value << 8) | std::to_integer<uint8_t>(b);
    return value;
}

uint32_t hash32(std::string_view s);

std::string macToString(const std::array<uint8_t, 6>& mac);
bool macFromString(std::string_view s, std::array<uint8_t, 6>& out);

std::array<uint8_t, 6> fakeMacFor(std::string_view deviceId);

uint64_t nowMs();

std::string urlDecode(std::string_view s);

}

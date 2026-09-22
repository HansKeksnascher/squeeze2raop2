#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace squeeze2raop2 {

// On-the-wire byte order of a fixed-width integer field. The bridge speaks a
// fixed mix: slimproto and AIFF are big-endian; WAV/RIFF is little-endian.
// These helpers convert between the host's native order and the wire order
// without reinterpret_cast or union type-punning: std::bit_cast is the
// standard-blessed conversion and has no padding hazard for std::array.
enum class Endian { Big, Little };

namespace detail {

// Return `value` reordered so that its object representation, read in native
// order, equals the wire bytes. Byte reversal is its own inverse, so this
// doubles as the inverse conversion (see readInt).
template <std::unsigned_integral T>
[[nodiscard]] constexpr T toWire(T value, Endian wire) {
    const bool wireIsNative = (wire == Endian::Big) ? (std::endian::native == std::endian::big)
                                                    : (std::endian::native == std::endian::little);
    if (wireIsNative) return value;
    std::array<std::byte, sizeof(T)> bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    for (size_t i = 0; i < sizeof(T) / 2; ++i) {
        const std::byte tmp = bytes[i];
        bytes[i] = bytes[sizeof(T) - 1 - i];
        bytes[sizeof(T) - 1 - i] = tmp;
    }
    return std::bit_cast<T>(bytes);
}

}  // namespace detail

// Read `sizeof(T)` bytes at `src` as an unsigned integer in `wire` order.
// The caller guarantees the range is readable (PacketReader enforces it).
template <Endian wire, std::unsigned_integral T>
[[nodiscard]] constexpr T readInt(const std::byte* src) {
    std::array<std::byte, sizeof(T)> bytes{};
    for (size_t i = 0; i < sizeof(T); ++i) bytes[i] = src[i];
    return detail::toWire(std::bit_cast<T>(bytes), wire);
}

// Write `value` as `sizeof(T)` bytes at `dst` in `wire` order.
template <Endian wire, std::unsigned_integral T>
constexpr void writeInt(std::byte* dst, T value) {
    const std::array<std::byte, sizeof(T)> bytes =
        std::bit_cast<std::array<std::byte, sizeof(T)>>(detail::toWire(value, wire));
    for (size_t i = 0; i < sizeof(T); ++i) dst[i] = bytes[i];
}

}  // namespace squeeze2raop2
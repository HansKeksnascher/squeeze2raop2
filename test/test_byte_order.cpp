// Pins the on-wire byte order of the shared endian helpers. The assertions
// check the exact emitted bytes (not just round-trips), so the swap logic is
// validated independently of the host's native byte order.

#include "byte_order.h"

#include "check.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace sq2t;
using squeeze2raop2::Endian;
using squeeze2raop2::readInt;
using squeeze2raop2::writeInt;

namespace {

void testBigEndian() {
    std::array<std::byte, 4> b{};
    writeInt<Endian::Big>(b.data(), uint32_t{0x11223344});
    expect(b[0] == std::byte{0x11} && b[1] == std::byte{0x22} && b[2] == std::byte{0x33} &&
               b[3] == std::byte{0x44},
           "big-endian byte order");
    expect(readInt<Endian::Big, uint32_t>(b.data()) == 0x11223344u, "big-endian round trip");
}

void testLittleEndian() {
    std::array<std::byte, 2> b{};
    writeInt<Endian::Little>(b.data(), uint16_t{0x1234});
    expect(b[0] == std::byte{0x34} && b[1] == std::byte{0x12}, "little-endian byte order");
    expect(readInt<Endian::Little, uint16_t>(b.data()) == 0x1234, "little-endian round trip");
}

void testWidths() {
    std::array<std::byte, 1> one{};
    writeInt<Endian::Big>(one.data(), uint8_t{0xAB});
    expect(one[0] == std::byte{0xAB}, "8-bit big-endian");
    expect(readInt<Endian::Little, uint8_t>(one.data()) == 0xAB, "8-bit little-endian");

    std::array<std::byte, 8> eight{};
    const uint64_t v = 0x0102030405060708ULL;
    writeInt<Endian::Big>(eight.data(), v);
    expect(eight.front() == std::byte{0x01} && eight.back() == std::byte{0x08},
           "64-bit big-endian");
    expect(readInt<Endian::Big, uint64_t>(eight.data()) == v, "64-bit round trip");
}

}  // namespace

int main() {
    testBigEndian();
    testLittleEndian();
    testWidths();
    std::printf("ok\n");
    return 0;
}
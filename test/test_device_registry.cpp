// Regression test for the mDNS `features` TXT parsing. AirPlay advertises a
// comma-separated pair of 32-bit hex words, low word first. The original
// std::from_chars(...,16) call rejected the "0x" prefix and stopped at the
// comma, so every receiver parsed as features=0 and was classified AP1.

#include "check.h"
#include "device_registry.h"

#include <map>
#include <string>

using namespace squeeze2raop2;
using sq2t::expect;

namespace {

AirplayDevice registerAirplay(const std::map<std::string, std::string>& txt) {
    DeviceRegistry registry;
    AirplayDevice seen;
    registry.setCallback([&](DeviceRegistry::Event ev, const AirplayDevice& d) {
        if (ev == DeviceRegistry::Event::Added) seen = d;
    });
    registry.onAirplayV4("Instance", "10.0.0.1", 7000, txt);
    return seen;
}

void testTwoWordFeatures() {
    // HomePod (AudioAccessory5,1): low word first, both HK pairing bits set.
    AirplayDevice hp = registerAirplay({{"features", "0x4A7FCA00,0x3C354BD0"}});
    expect(hp.features == 0x3C354BD04A7FCA00ULL, "homepod features combine low|high<<32");
    expect(hp.airplay2(), "homepod classified AP2");

    // Sonos: third-party AirPlay 2 speaker, same layout.
    AirplayDevice sonos = registerAirplay({{"features", "0x445F8A00,0x801C340"}});
    expect(sonos.features == 0x0801C340445F8A00ULL, "sonos features combine low|high<<32");
    expect(sonos.airplay2(), "sonos classified AP2");
}

void testPrefixAndCase() {
    AirplayDevice upper = registerAirplay({{"features", "0x4A7FCA00,0x3C354BD0"}});
    AirplayDevice lower = registerAirplay({{"features", "0x4a7fca00,0x3c354bd0"}});
    AirplayDevice bare  = registerAirplay({{"features", "4a7fca00,3c354bd0"}});
    expect(upper.features == lower.features, "hex case-insensitive");
    expect(upper.features == bare.features, "0x prefix optional");
}

void testSingleWordAndGarbage() {
    // A lone low word carries no HK bits -> not AP2 (bit 38/48 live in the
    // high word).
    AirplayDevice one = registerAirplay({{"features", "0x4A7FCA00"}});
    expect(one.features == 0x4A7FCA00ULL, "single word stays in the low half");
    expect(!one.airplay2(), "single low word is not AP2");

    // strtoull semantics: leading hex digits parse, garbage yields 0.
    AirplayDevice junk = registerAirplay({{"features", "zzzz"}});
    expect(junk.features == 0, "non-hex features parse as 0");
    expect(!junk.airplay2(), "garbage features is not AP2");

    // Missing key: leave the default (0).
    AirplayDevice none = registerAirplay({{"model", "Whatever"}});
    expect(none.features == 0, "missing features stays 0");
}

}  // namespace

int main() {
    testTwoWordFeatures();
    testPrefixAndCase();
    testSingleWordAndGarbage();
    return 0;
}

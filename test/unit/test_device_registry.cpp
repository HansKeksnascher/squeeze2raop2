// Regression test for the mDNS `features` TXT parsing. AirPlay advertises a
// comma-separated pair of 32-bit hex words, low word first. The original
// std::from_chars(...,16) call rejected the "0x" prefix and stopped at the
// comma, so every receiver parsed as features=0 and was classified AP1.

#include "discovery/device_registry.h"

#include "check.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace squeeze2raop2;
using squeeze2raop2::test::expect;
using squeeze2raop2::test::require;

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

}  // namespace

SQ2_TEST(registry, two_word_features) {
    // HomePod (AudioAccessory5,1): low word first, both HK pairing bits set.
    AirplayDevice hp = registerAirplay({{"features", "0x4A7FCA00,0x3C354BD0"}});
    expect(hp.features == 0x3C354BD04A7FCA00ULL, "homepod features combine low|high<<32");
    expect(hp.airplay2(), "homepod classified AP2");

    // Sonos: third-party AirPlay 2 speaker, same layout.
    AirplayDevice sonos = registerAirplay({{"features", "0x445F8A00,0x801C340"}});
    expect(sonos.features == 0x0801C340445F8A00ULL, "sonos features combine low|high<<32");
    expect(sonos.airplay2(), "sonos classified AP2");
}

SQ2_TEST(registry, prefix_and_case) {
    AirplayDevice upper = registerAirplay({{"features", "0x4A7FCA00,0x3C354BD0"}});
    AirplayDevice lower = registerAirplay({{"features", "0x4a7fca00,0x3c354bd0"}});
    AirplayDevice bare = registerAirplay({{"features", "4a7fca00,3c354bd0"}});
    expect(upper.features == lower.features, "hex case-insensitive");
    expect(upper.features == bare.features, "0x prefix optional");
}

SQ2_TEST(registry, single_word_and_garbage) {
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

SQ2_TEST(registry, raop_then_airplay_updates_transport) {
    // The two services of one receiver race. If the raop record lands first the
    // device is Added without features (not AP2); the airplay record must then
    // update it to AP2 and merge both services under one key.
    DeviceRegistry registry;
    std::vector<std::pair<DeviceRegistry::Event, AirplayDevice>> events;
    registry.setCallback(
        [&](DeviceRegistry::Event ev, const AirplayDevice& d) { events.push_back({ev, d}); });

    registry.onRaopV4("6A329C251848@Küche", "10.0.0.9", 7000, {{"am", "AudioAccessory5,1"}});
    registry.onAirplayV4("6A329C251848@Küche", "10.0.0.9", 7000,
                         {{"features", "0x4A7FCA00,0x3C354BD0"}, {"pk", "present"}});

    require(events.size() == 2, "added then updated");
    expect(events[0].first == DeviceRegistry::Event::Added, "raop record first is Added");
    expect(!events[0].second.airplay2(), "raop-only is not AP2 yet");
    expect(events[1].first == DeviceRegistry::Event::Updated, "airplay record is Updated");
    expect(events[1].second.airplay2(), "airplay record flips it to AP2");
    expect(events[1].second.hasAirplay() && events[1].second.hasRaop(),
           "both services merged under one device");
}

SQ2_TEST(registry, transport_selection) {
    // Pure helper coverage for the AP2/AP1 and port decision.
    AirplayDevice d;
    d.raopPort = 7000;
    expect(!d.useAirplay2(), "raop-only is AP1");
    expect(d.preferredPort() == 7000, "raop-only uses the raop port");

    d.airplayPort = 7100;  // airplay record seen but no HK bits yet
    expect(!d.useAirplay2(), "airplay without HK bits stays AP1");
    expect(d.preferredPort() == 7000, "falls back to the raop port");

    d.features = 1ULL << 38;
    expect(d.useAirplay2(), "HK bit flips it to AP2");
    expect(d.preferredPort() == 7100, "AP2 uses the airplay port");

    AirplayDevice a;
    a.airplayPort = 7100;
    a.features = 1ULL << 48;
    expect(a.useAirplay2(), "airplay-only with HK bit is AP2");
    expect(a.preferredPort() == 7100, "airplay-only uses the airplay port");
}
#include "app/config.h"

#include "check.h"

#include <array>
#include <string>

using namespace squeeze2raop2::test;
using squeeze2raop2::Args;
using squeeze2raop2::parseArgs;
using squeeze2raop2::PlayerConfig;
using squeeze2raop2::ResolvedPlayerConfig;
using squeeze2raop2::resolvePlayer;
using squeeze2raop2::VolumeMode;

SQ2_TEST(config, builtins) {
    PlayerConfig defaults;
    PlayerConfig player;
    const ResolvedPlayerConfig r = resolvePlayer(defaults, player);
    expect(r.enabled, "[defaults] enabled is true");
    expect(r.airplay2, "[defaults] protocol ap2");
    expect(r.volumeMode == VolumeMode::Lms, "[defaults] volume lms");
    expect(r.volPct == 0.7f, "[defaults] volume-pct 0.7");
    expect(r.latencyMs == 500, "[defaults] latency 500");
    expect(r.paceRealtime, "[defaults] pace realtime");
    expect(!r.target.has_value(), "[defaults] no static target");
}

SQ2_TEST(config, default_inheritance) {
    PlayerConfig defaults;
    defaults.volumeMode = VolumeMode::Fixed;
    defaults.volPct = 40.0f;
    defaults.latencyMs = 1000;
    defaults.paceRealtime = false;
    defaults.password = "secret";

    PlayerConfig player;
    const ResolvedPlayerConfig inherited = resolvePlayer(defaults, player);
    expect(inherited.volumeMode == VolumeMode::Fixed, "inherits volume mode");
    expect(inherited.volPct == 40.0f, "inherits volume-pct");
    expect(inherited.latencyMs == 1000, "inherits latency");
    expect(!inherited.paceRealtime, "inherits pace");
    expect(inherited.password == "secret", "inherits password");

    PlayerConfig override;
    override.volPct = 55.0f;
    override.latencyMs = 250;
    const ResolvedPlayerConfig overridden = resolvePlayer(defaults, override);
    expect(overridden.volPct == 55.0f, "player overrides volume-pct");
    expect(overridden.latencyMs == 250, "player overrides latency");
    expect(overridden.volumeMode == VolumeMode::Fixed, "unoverridden still inherits");
}

SQ2_TEST(config, identity_and_target) {
    PlayerConfig defaults;
    PlayerConfig player;
    player.id = "542a1b5cc9e2";
    player.name = "Kueche15";
    std::array<uint8_t, 6> mac{0xaa, 1, 2, 3, 4, 5};
    player.mac = mac;
    player.targetHost = "192.168.1.157";
    player.targetPort = 7100;
    player.airplay2 = false;

    const ResolvedPlayerConfig r = resolvePlayer(defaults, player);
    expect(r.id == "542a1b5cc9e2", "id copied");
    expect(r.name == "Kueche15", "name copied");
    expect(r.key == "542a1b5cc9e2", "key prefers id");
    expect(r.explicitMac, "explicit mac flag");
    expect(r.mac == mac, "mac copied");
    expect(r.target.has_value() && r.target->first == "192.168.1.157", "target host");
    expect(r.target->second == 7100, "target port");
    expect(!r.airplay2, "protocol override");

    PlayerConfig nameOnly;
    nameOnly.name = "Kitchen";
    const ResolvedPlayerConfig n = resolvePlayer(defaults, nameOnly);
    expect(n.key == "Kitchen", "key falls back to name");
    expect(!n.explicitMac, "no explicit mac");
}

SQ2_TEST(config, parse_args) {
    {
        const char* argv[] = {"prog", "--config", "/tmp/x.conf"};
        int code = 7;
        auto args = parseArgs(3, const_cast<char**>(argv), code);
        expect(args.has_value(), "--config parsed");
        expect(args->configPath == "/tmp/x.conf", "--config path captured");
    }
    {
        const char* argv[] = {"prog"};
        int code = 7;
        auto args = parseArgs(1, const_cast<char**>(argv), code);
        expect(args.has_value(), "default config path");
        expect(args->configPath == "squeeze2raop2.conf", "default path is cwd config");
    }
    {
        const char* argv[] = {"prog", "--bogus"};
        int code = 0;
        auto args = parseArgs(2, const_cast<char**>(argv), code);
        expect(!args.has_value(), "unknown option rejected");
        expect(code == 1, "unknown option exit code");
    }
}
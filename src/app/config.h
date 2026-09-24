#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/log.h"
#include "playback/volume_map.h"

namespace squeeze2raop2 {

enum class VolumeMode {
    Lms,    // follow the LMS slider: the AUDG-recovered slider percent passes
            // straight to the receiver (LMS minimum = receiver mute, LMS
            // maximum = 0 dB; 0.3 dB per slider step)
    Fixed,  // ignore AUDG, play every session at volPct
};

// [global]: process-wide settings. Concrete values with built-in defaults.
struct GlobalConfig {
    // connect to this LMS; absent => UDP discovery on lmsPort.
    std::optional<std::string> lmsHost;
    uint16_t lmsPort = 3483;
    bool discovery = true;
    std::string mdnsIface;
    bool mdnsDebug = false;
    log::Level logLevel = log::Level::Info;
    // reconnect when the LMS control connection is silent for this long.
    uint32_t serverTimeoutMs = 35000;
    // end a stream whose HTTP source delivers nothing for this long (0 = off);
    // catches a stalled/half-open stream that never yields EOF or a socket error.
    uint32_t sourceTimeoutMs = 15000;
    // Direct HTTPS streams: verify the station certificate against the system
    // trust store (or tlsCaPath) and fail closed. tlsVerify=off skips it.
    bool tlsVerify = true;
    std::string tlsCaPath;  // empty = autodetect; a CA bundle file or certs dir
    // create a section (auto = true) for every discovered device that has none.
    bool autoRegister = true;
    // Report receiver-initiated volume changes (HomePod/Sonos volume buttons)
    // back to LMS by nudging its volume mixer. Off disables the feedback.
    bool volumeFeedback = true;

    std::string configPath = "squeeze2raop2.conf";
};

// [default] and each [player "..."] section. Every field is optional: an unset
// field inherits from the level below (built-ins, then [default], then the
// player). Identity and target keys are rejected in [default].
struct PlayerConfig {
    // identity
    std::optional<std::string> id;  // 12-hex mDNS device id
    std::optional<std::string> name;
    std::optional<std::array<uint8_t, 6>> mac;

    // target
    std::optional<std::string> targetHost;
    std::optional<uint16_t> targetPort;
    std::optional<bool> airplay2;  // protocol ap2 (true) / ap1 (false)
    std::optional<std::string> password;

    // behavior
    std::optional<bool> enabled;
    std::optional<VolumeMode> volumeMode;
    std::optional<std::string> volumeMap;
    std::optional<float> volPct;
    std::optional<int> latencyMs;
    std::optional<std::string> sinkPath;
    std::optional<bool> paceRealtime;

    // machine-managed state; only meaningful in [player] sections.
    std::optional<std::string> creds;
    // discovery-created section: spawn while discovered, not at startup.
    std::optional<bool> autoSection;
};

// A parsed section after inheritance has been applied: no optionals, safe to
// hand to SessionManager/PlayerSession.
struct ResolvedPlayerConfig {
    std::string key;  // section key used for creds lookup (id, else name)
    std::string id;
    std::string name;
    std::array<uint8_t, 6> mac{};
    bool explicitMac = false;
    bool enabled = true;
    bool autoRegistered = false;
    std::optional<std::pair<std::string, uint16_t>> target;  // static target
    bool airplay2 = true;
    std::string password;
    VolumeMode volumeMode = VolumeMode::Lms;
    std::string volumeMap = kDefaultVolumeMap;
    float volPct = 0.7f;
    int latencyMs = 500;
    std::optional<std::string> sinkPath;
    bool paceRealtime = true;
};

// Fully parsed configuration: [global], the raw [default] section, and every
// section resolved through it.
struct Settings {
    GlobalConfig global;
    PlayerConfig defaults;
    std::vector<ResolvedPlayerConfig> players;
};

// Merge built-ins -> [default] -> player. Identity/target come from `player`
// only; behavioral fields fall back to `defaults`.
ResolvedPlayerConfig resolvePlayer(const PlayerConfig& defaults, const PlayerConfig& player);

// Command-line surface: --config <path>, -h/--help, -V/--version. Behavior
// lives in the config file.
struct Args {
    std::string configPath = "squeeze2raop2.conf";
};

std::optional<Args> parseArgs(int argc, char** argv, int& exitCode);

}  // namespace squeeze2raop2
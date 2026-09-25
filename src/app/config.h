#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "airplay/raop_types.h"
#include "common/log.h"
#include "lms/slimproto_protocol.h"
#include "playback/volume_map.h"

namespace squeeze2raop2 {

// --- Built-in defaults -----------------------------------------------------
//
// Single source of truth for the resolved player config; config.cpp and
// persistence.cpp read these instead of repeating the literals.

constexpr uint32_t kDefaultServerTimeoutMs = 35000;
constexpr uint32_t kDefaultSourceTimeoutMs = 15000;
constexpr uint16_t kDefaultTargetPort = kDefaultRaopPort;
constexpr int kDefaultLatencyMs = kDefaultAirplayLatencyMs;
constexpr float kDefaultVolumePct = 0.7f;
constexpr bool kDefaultDiscovery = true;
constexpr bool kDefaultAutoRegister = true;
constexpr bool kDefaultVolumeFeedback = true;
constexpr bool kDefaultTlsVerify = true;
constexpr const char* kDefaultConfigPath = "squeeze2raop2.conf";

// Accepted ranges for persisted numeric settings.
constexpr uint32_t kServerTimeoutMinMs = 1000;
constexpr uint32_t kServerTimeoutMaxMs = 600000;
constexpr uint32_t kSourceTimeoutMinMs = 1000;
constexpr uint32_t kSourceTimeoutMaxMs = 600000;
constexpr float kVolumePctMin = 0.5f;
constexpr float kVolumePctMax = 100.f;
constexpr int kLatencyMinMs = 250;
constexpr int kLatencyMaxMs = 2000;

// --- Config file surface ---------------------------------------------------
//
// INI section/key/value spellings, shared by the parser and the generated
// template so they cannot drift.

constexpr const char* kSectionGlobal = "global";
constexpr const char* kSectionDefault = "default";
constexpr const char* kSectionPlayer = "player";

constexpr const char* kKeyLms = "lms";
constexpr const char* kKeyDiscovery = "discovery";
constexpr const char* kKeyIface = "iface";
constexpr const char* kKeyMdnsDebug = "mdns-debug";
constexpr const char* kKeyAutoRegister = "auto-register";
constexpr const char* kKeyVolumeFeedback = "volume-feedback";
constexpr const char* kKeyServerTimeoutMs = "server-timeout-ms";
constexpr const char* kKeySourceTimeoutMs = "source-timeout-ms";
constexpr const char* kKeyTlsVerify = "tls-verify";
constexpr const char* kKeyTlsCa = "tls-ca";
constexpr const char* kKeyLog = "log";

constexpr const char* kKeyId = "id";
constexpr const char* kKeyMac = "mac";
constexpr const char* kKeyName = "name";
constexpr const char* kKeyTarget = "target";
constexpr const char* kKeyProtocol = "protocol";
constexpr const char* kKeyPassword = "password";
constexpr const char* kKeyEnabled = "enabled";
constexpr const char* kKeyVolume = "volume";
constexpr const char* kKeyVolumeMap = "volume-map";
constexpr const char* kKeyVolumePct = "volume-pct";
constexpr const char* kKeyLatencyMs = "latency-ms";
constexpr const char* kKeySink = "sink";
constexpr const char* kKeyPace = "pace";
constexpr const char* kKeyCreds = "creds";
constexpr const char* kKeyAuto = "auto";

// Keys the program owns and rewrites in place.
constexpr const char* kManagedKeys[] = {kKeyMac, kKeyCreds, kKeyName};

// Boolean spellings accepted by the parser.
constexpr const char* kTrueValues[] = {"on", "true", "yes", "1"};
constexpr const char* kFalseValues[] = {"off", "false", "no", "0"};

// Enum spellings.
constexpr const char* kProtocolAp1 = "ap1";
constexpr const char* kProtocolAp2 = "ap2";
constexpr const char* kVolumeModeLms = "lms";
constexpr const char* kVolumeModeFixed = "fixed";
constexpr const char* kPaceFast = "fast";
constexpr const char* kPaceRealtime = "realtime";

// Legacy single-file state suffix (imported on first run).
constexpr const char* kLegacyStateSuffix = ".state";

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
    uint16_t lmsPort = kDefaultLmsPort;
    bool discovery = kDefaultDiscovery;
    std::string mdnsIface;
    bool mdnsDebug = false;
    log::Level logLevel = log::Level::Info;
    // reconnect when the LMS control connection is silent for this long.
    uint32_t serverTimeoutMs = kDefaultServerTimeoutMs;
    // end a stream whose HTTP source delivers nothing for this long (0 = off);
    // catches a stalled/half-open stream that never yields EOF or a socket error.
    uint32_t sourceTimeoutMs = kDefaultSourceTimeoutMs;
    // Direct HTTPS streams: verify the station certificate against the system
    // trust store (or tlsCaPath) and fail closed. tlsVerify=off skips it.
    bool tlsVerify = kDefaultTlsVerify;
    std::string tlsCaPath;  // empty = autodetect; a CA bundle file or certs dir
    // create a section (auto = true) for every discovered device that has none.
    bool autoRegister = kDefaultAutoRegister;
    // Report receiver-initiated volume changes (HomePod/Sonos volume buttons)
    // back to LMS by nudging its volume mixer. Off disables the feedback.
    bool volumeFeedback = kDefaultVolumeFeedback;

    std::string configPath = kDefaultConfigPath;
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
    float volPct = kDefaultVolumePct;
    int latencyMs = kDefaultLatencyMs;
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
    std::string configPath = kDefaultConfigPath;
};

std::optional<Args> parseArgs(int argc, char** argv, int& exitCode);

}  // namespace squeeze2raop2
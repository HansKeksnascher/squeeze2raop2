#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace squeeze2raop2 {

// RAOP/sender defaults, shared by config resolution and the sender binding.
constexpr uint16_t kDefaultRaopPort = 7000;
constexpr int kDefaultAirplayLatencyMs = 500;
// Sender ring capacity in interleaved int16 samples (~2.97 s at 44.1 kHz).
constexpr size_t kRingCapacitySamples = 1 << 18;
// Spoofed Apple device model reported to the receiver.
constexpr const char* kSenderIdentity = "iPhone14,3";
// Prefix stripped from the vendored sender's forwarded log lines.
constexpr const char* kSenderLogPrefix = "Cast: ";
// Max single pump() wait so a setter thread never blocks long on the host.
constexpr int kSenderPumpMaxWaitMs = 20;
// Between-tracks keep-alive pump period.
constexpr int kKeepAlivePeriodMs = 100;

// One receiver to connect to, plus the credentials recovered from a previous
// pairing (the storedCreds JSON drives the HAP pin flow).
struct RaopTarget {
    std::string host;
    uint16_t port = kDefaultRaopPort;
    bool airplay2 = true;
    std::string password;
    std::string storedCreds;
};

// Reports long-term pairing credentials recovered by the sender, so the bridge
// can persist them (deviceId, credsJson).
using CredentialSink = std::function<void(const std::string&, const std::string&)>;

// Reports a player rename sent by LMS ('setd'), so the bridge can persist it.
using NameSink = std::function<void(const std::string&)>;

}  // namespace squeeze2raop2
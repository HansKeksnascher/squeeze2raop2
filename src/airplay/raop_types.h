#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace squeeze2raop2 {

// One receiver to connect to, plus the credentials recovered from a previous
// pairing (the storedCreds JSON drives the HAP pin flow).
struct RaopTarget {
    std::string host;
    uint16_t port = 7000;
    bool airplay2 = true;
    std::string password;
    std::string storedCreds;
};

// Reports long-term pairing credentials recovered by the sender, so the bridge
// can persist them (deviceId, credsJson).
using CredentialSink = std::function<void(const std::string&, const std::string&)>;

}  // namespace squeeze2raop2

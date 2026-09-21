#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "log.h"

namespace sq2 {

struct PlayerSettings {
    std::string deviceId;
    std::string name;
    std::array<uint8_t, 6> mac{};
    bool explicitMac = false;
    bool explicitName = false;
};

struct Settings {
    std::optional<std::string> lmsHost;
    uint16_t lmsPort = 3483;
    std::vector<PlayerSettings> players;
    std::vector<std::pair<std::string, std::string>> staticDevices;
    std::optional<std::string> sinkPath;
    bool paceRealtime = true;
    log::Level logLevel = log::Level::Info;
    std::string statePath = "sqraop2.state";
    std::string mdnsIface;
    bool mdnsDebug = false;
    bool discovery = true;
    // Fixed AirPlay volume percent sent to receivers (bypasses the LMS mixer
    // mapping while volume control is being designed). Maps onto AirPlay's
    // -30..0 dB protocol range via db = 0.3*pct - 30. 0 would be the receiver
    // mute sentinel (-144 dB), so the floor is 0.5.
    float volPct = 0.7f;

    struct {
        std::string host;
        std::string name;
        uint16_t port = 7000;
        bool enabled = false;
        bool airplay2 = true;
        std::string password;
    } ap;
};

std::optional<Settings> parseCommandLine(int argc, char** argv, int& exitCode);

}

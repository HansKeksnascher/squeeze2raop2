#pragma once

#include "app/config.h"
#include "app/persistence.h"
#include "discovery/device_registry.h"
#include "playback/player_session.h"
#include "playback/volume_map.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace squeeze2raop2 {

// Owns one PlayerSession per known player and keeps their AirPlay targets in
// sync with the registry. Each session is built from the per-player section
// resolved out of Persistence; onRegistryEvent() runs on the DeviceRegistry's
// thread and the mutex_ serializes it against concurrent registry events.
class SessionManager {
public:
    SessionManager(const Settings& settings, Persistence& persistence);

    void onRegistryEvent(DeviceRegistry::Event ev, const AirplayDevice& dev);

private:
    VolumeAnchors anchorsFor(const ResolvedPlayerConfig& cfg) const;

    const Settings& settings_;
    Persistence& persistence_;
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<PlayerSession>> sessions_;
};

}  // namespace squeeze2raop2
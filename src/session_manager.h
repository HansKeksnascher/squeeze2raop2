#pragma once

#include "config.h"
#include "device_registry.h"
#include "player_session.h"
#include "state_store.h"
#include "volume_map.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace squeeze2raop2 {

// Owns one PlayerSession per known device and keeps their AirPlay targets
// in sync with the registry. onRegistryEvent() runs on the DeviceRegistry's
// thread; the mutex_ serializes it against concurrent registry events.
class SessionManager {
public:
    SessionManager(const Settings& settings, StateStore& store);

    void onRegistryEvent(DeviceRegistry::Event ev, const AirplayDevice& dev);

private:
    const Settings& settings_;
    StateStore& store_;
    VolumeAnchors anchors_;
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<PlayerSession>> sessions_;
};

}  // namespace squeeze2raop2

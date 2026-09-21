#include "session_manager.h"

#include "log.h"
#include "util.h"

#include <optional>
#include <utility>

namespace squeeze2raop2 {

SessionManager::SessionManager(const Settings& settings, StateStore& store)
    : settings_(settings), store_(store),
      // parseCommandLine already validated the spec; the fallback is
      // unreachable but keeps programmatic Settings safe
      anchors_(VolumeAnchors::parse(settings.volumeMap)
                   .value_or(*VolumeAnchors::parse(kDefaultVolumeMap))) {}

void SessionManager::onRegistryEvent(DeviceRegistry::Event ev, const AirplayDevice& dev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ev == DeviceRegistry::Event::Removed) {
        auto it = sessions_.find(dev.id);
        if (it == sessions_.end()) return;
        log::info("session closed: {} ({})", dev.name, dev.id);
        it->second.reset();   // ~PlayerSession stops client + stream
        sessions_.erase(it);
        return;
    }
    if (ev == DeviceRegistry::Event::Updated) {
        auto it = sessions_.find(dev.id);
        if (it == sessions_.end()) return;
        if (!dev.host.empty() && (dev.hasRaop() || dev.hasAirplay())) {
            RaopTarget t;
            t.host = dev.host;
            t.port = dev.airplay2() ? (dev.airplayPort ? dev.airplayPort : dev.raopPort)
                                    : (dev.raopPort ? dev.raopPort : dev.airplayPort);
            t.airplay2 = dev.airplay2();
            t.password = settings_.ap.password;
            t.storedCreds = store_.credsFor(dev.id).value_or(std::string());
            it->second->updateTarget(t);
            if (t.port)
                log::debug("session target refreshed: {} {}:{}",
                           dev.name, t.host, t.port);
        }
        return;
    }
    if (ev != DeviceRegistry::Event::Added) return;
    if (sessions_.count(dev.id)) return;

    bool assigned = false;
    std::array<uint8_t, 6> mac = store_.macFor(dev.id, assigned);
    std::optional<std::string> sinkPath;
    if (settings_.sinkPath) {
        std::string base = *settings_.sinkPath;
        size_t dot = base.find_last_of('.');
        std::string stem = (dot != std::string::npos) ? base.substr(0, dot) : base;
        std::string ext = (dot != std::string::npos) ? base.substr(dot) : "";
        sinkPath = stem + "-" + dev.id + ext;
    }
    log::info("session created: {} mac={} ({} {}:{}{}{})",
              dev.name, macToString(mac), dev.airplay2() ? "ap2" : "ap1",
              dev.host, dev.airplay2() ? dev.airplayPort : dev.raopPort,
              dev.pw ? " password" : "", assigned ? " new-mac" : "");

    std::optional<RaopTarget> raopTarget;
    if (settings_.ap.enabled) {
        RaopTarget target;
        target.host = settings_.ap.host;
        target.port = settings_.ap.port;
        target.airplay2 = settings_.ap.airplay2;
        target.password = settings_.ap.password;
        target.storedCreds = store_.credsFor(dev.id).value_or(std::string());
        raopTarget = target;
    } else if (!dev.host.empty()) {
        RaopTarget target;
        target.host = dev.host;
        target.port = dev.airplay2() ? dev.airplayPort : dev.raopPort;
        target.airplay2 = dev.airplay2();
        target.password = settings_.ap.password;
        target.storedCreds = store_.credsFor(dev.id).value_or(std::string());
        raopTarget = target;
    }

    auto session = std::make_unique<PlayerSession>(
        dev.id, dev.name, mac, settings_.lmsHost, settings_.lmsPort,
        settings_.paceRealtime, sinkPath, raopTarget,
        [this, devId = dev.id](const std::string& deviceId, const std::string& creds) {
            (void)deviceId;
            store_.saveCreds(devId, creds);
        },
        settings_.volumeMode, anchors_, settings_.volPct, settings_.apLatencyMs);
    session->start();
    sessions_[dev.id] = std::move(session);
}

} // namespace squeeze2raop2

#include "playback/session_manager.h"

#include "airplay/raop_types.h"
#include "common/log.h"
#include "common/util.h"

#include <optional>
#include <utility>

namespace squeeze2raop2 {

SessionManager::SessionManager(const Settings& settings, Persistence& persistence)
    : settings_(settings), persistence_(persistence) {}

VolumeAnchors SessionManager::anchorsFor(const ResolvedPlayerConfig& cfg) const {
    // The loader already validated the spec; the fallback keeps programmatic
    // configs safe.
    if (auto parsed = VolumeAnchors::parse(cfg.volumeMap)) return *parsed;
    log::warn("volume map '{}' invalid; using default", cfg.volumeMap);
    if (auto fallback = VolumeAnchors::parse(kDefaultVolumeMap)) return *fallback;
    return VolumeAnchors{};
}

void SessionManager::onRegistryEvent(DeviceRegistry::Event ev, const AirplayDevice& dev) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto makeTarget = [&](const std::string& host, uint16_t port, bool airplay2,
                          const ResolvedPlayerConfig& cfg) {
        RaopTarget t;
        t.host = host;
        t.port = port;
        t.airplay2 = airplay2;
        t.password = cfg.password;
        t.storedCreds = persistence_.credsFor(cfg.key).value_or(std::string());
        return t;
    };

    if (ev == DeviceRegistry::Event::Removed) {
        auto resolved = persistence_.resolve(dev.id, dev.name, /*autoRegister=*/false);
        if (!resolved) return;
        auto it = sessions_.find(resolved->key);
        if (it == sessions_.end()) return;
        log::info("session closed: {} ({})", dev.name, dev.id);
        it->second.reset();  // ~PlayerSession stops client + stream
        sessions_.erase(it);
        return;
    }

    // Identity/target resolution happens once for both the Added and the
    // transport-change Updated path; auto-registration only for a new device.
    auto resolved = persistence_.resolve(
        dev.id, dev.name, ev == DeviceRegistry::Event::Added && settings_.global.autoRegister);
    if (!resolved) return;

    // Prefer native AirPlay 2 when the `_airplay._tcp` record is present and
    // advertises the HK bits; otherwise classic RAOP. The two mDNS records for
    // one receiver race, so a session first built from the raop record (no
    // features yet) may need to be rebuilt once the airplay record lands.
    const bool ap2 = dev.useAirplay2();
    const uint16_t port = dev.preferredPort();

    if (ev == DeviceRegistry::Event::Updated) {
        auto it = sessions_.find(resolved->key);
        if (it == sessions_.end()) return;
        if (resolved->target) return;  // a user-pinned target wins
        if (dev.host.empty() || (!dev.hasRaop() && !dev.hasAirplay())) return;
        if (it->second->airplay2() == ap2) {
            RaopTarget t = makeTarget(dev.host, port, ap2, *resolved);
            it->second->updateTarget(t);
            if (t.port) log::debug("session target refreshed: {} {}:{}", dev.name, t.host, t.port);
            return;
        }
        log::info("session transport changed for {} ({} -> {}); rebuilding",
                  resolved->name.empty() ? resolved->key : resolved->name,
                  it->second->airplay2() ? "ap2" : "ap1", ap2 ? "ap2" : "ap1");
        it->second.reset();
        sessions_.erase(it);
        // fall through and recreate with the correct transport
    } else if (ev != DeviceRegistry::Event::Added) {
        return;
    }

    if (!resolved->enabled) {
        log::info("session skipped: {} (disabled)", dev.name);
        return;
    }
    if (sessions_.count(resolved->key)) return;

    std::optional<std::string> sinkPath;
    if (resolved->sinkPath) {
        std::string base = *resolved->sinkPath;
        const size_t dot = base.find_last_of('.');
        const std::string stem = (dot != std::string::npos) ? base.substr(0, dot) : base;
        const std::string ext = (dot != std::string::npos) ? base.substr(dot) : "";
        sinkPath = stem + "-" + resolved->key + ext;
    }

    std::optional<RaopTarget> raopTarget;
    if (resolved->target)
        raopTarget = makeTarget(resolved->target->first, resolved->target->second,
                                resolved->airplay2, *resolved);
    else if (!dev.host.empty() && (dev.hasRaop() || dev.hasAirplay()))
        raopTarget = makeTarget(dev.host, port, ap2, *resolved);

    log::info("session created: {} mac={} ({} {}:{}{})", resolved->name, macToString(resolved->mac),
              (raopTarget ? raopTarget->airplay2 : resolved->airplay2) ? "ap2" : "ap1",
              raopTarget ? raopTarget->host : dev.host, raopTarget ? raopTarget->port : 0,
              resolved->password.empty() ? "" : " password");

    const std::string key = resolved->key;
    auto session = std::make_unique<PlayerSession>(
        *resolved, settings_.global, anchorsFor(*resolved), sinkPath, raopTarget,
        [this, key](const std::string& deviceId, const std::string& creds) {
            (void)deviceId;
            persistence_.saveCreds(key, creds);
        },
        [this, key](const std::string& name) { persistence_.savePlayerName(key, name); });
    session->start();
    sessions_[key] = std::move(session);
}

}  // namespace squeeze2raop2
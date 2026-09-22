// squeeze2raop2 - bridge between LMS slimproto and AirPlay receivers.
// This file wires the process together: signals, state store, the device
// registry and mDNS discovery. Per-device streaming lives in
// player_session.cpp; session lifecycle in session_manager.cpp.

#include "config.h"
#include "device_registry.h"
#include "log.h"
#include "mdns.h"
#include "session_manager.h"
#include "shutdown_flag.h"
#include "state_store.h"

#include <chrono>
#include <string>
#include <thread>

namespace squeeze2raop2 {

void runBridge(const Settings& settings) {
    installShutdownSignalHandlers();

    StateStore store;
    std::string error;
    if (!store.open(settings.statePath, error)) {
        log::error("state store: {}", error);
        return;
    }

    DeviceRegistry registry;
    SessionManager manager(settings, store);
    registry.setCallback([&manager](DeviceRegistry::Event ev, const AirplayDevice& dev) {
        manager.onRegistryEvent(ev, dev);
    });

    for (const auto& [id, name] : settings.staticDevices) {
        AirplayDevice dev;
        dev.id = id;
        dev.name = name;
        manager.onRegistryEvent(DeviceRegistry::Event::Added, dev);
    }

    const PlayerSettings& mainPlayer = settings.players.front();
    if (settings.staticDevices.empty() &&
        (mainPlayer.explicitName || mainPlayer.explicitMac || settings.sinkPath)) {
        AirplayDevice dev;
        dev.id = mainPlayer.deviceId;
        dev.name = mainPlayer.name;
        manager.onRegistryEvent(DeviceRegistry::Event::Added, dev);
    }

    if (settings.mdnsDebug) {
        MdnsBrowser browser;
        MdnsBrowser::RecordCallback cb = [](const MdnsRecord& rec, MdnsBrowser::RecordEvent ev) {
            const char* what = (ev == MdnsBrowser::RecordEvent::Added) ? "added" : "removed";
            log::info("mdns[{}] {} {} -> {}\n  txt:", what, rec.type, rec.instance,
                      rec.port ? rec.host + ":" + std::to_string(rec.port) : std::string());
            for (const auto& [k, v] : rec.txt) log::info("   {}={}", k, v);
        };
        if (!browser.start(settings.mdnsIface, cb, error)) {
            log::error("mdns: {}", error);
            return;
        }
        log::info("mdns debug mode: browsing (ctrl-c to exit)");
        while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        browser.stop();
        return;
    }

    if (!settings.discovery) {
        log::info("discovery disabled; running static devices only");
    }
    MdnsBrowser browser;
    MdnsBrowser::RecordCallback cb = [&registry](const MdnsRecord& rec,
                                                 MdnsBrowser::RecordEvent ev) {
        if (rec.type == "_raop._tcp") {
            if (ev == MdnsBrowser::RecordEvent::Added)
                registry.onRaopV4(rec.instance, rec.host, rec.port, rec.txt);
            else
                registry.onRaopGone(rec.instance);
        } else if (rec.type == "_airplay._tcp") {
            if (ev == MdnsBrowser::RecordEvent::Added)
                registry.onAirplayV4(rec.instance, rec.host, rec.port, rec.txt);
            else
                registry.onAirplayGone(rec.instance);
        }
    };
    if (settings.discovery) {
        if (!browser.start(settings.mdnsIface, cb, error)) {
            log::warn("mdns: {} (continuing without discovery)", error);
        }
    }

    log::info("running (ctrl-c to exit)");
    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    browser.stop();
    log::info("bye");
}

}  // namespace squeeze2raop2

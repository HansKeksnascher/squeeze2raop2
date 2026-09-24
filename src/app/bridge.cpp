// squeeze2raop2 - bridge between LMS slimproto and AirPlay receivers.
// This file wires the process together: signals, the config/state store, the
// device registry and mDNS discovery. Per-device streaming lives in
// playback/player_session.cpp; session lifecycle in session_manager.cpp.

#include "app/config.h"
#include "app/persistence.h"
#include "app/shutdown_flag.h"
#include "common/log.h"
#include "common/transport.h"
#include "discovery/device_registry.h"
#include "discovery/mdns.h"
#include "playback/session_manager.h"

#include <chrono>
#include <string>
#include <thread>

namespace squeeze2raop2 {

void runBridge(const Settings& settings, Persistence& persistence) {
    installShutdownSignalHandlers();

    // Set up the shared TLS trust context before any session can advertise
    // CanHTTPS=1. On failure the caps leave it out, so LMS keeps proxying
    // https streams instead of handing over URLs the bridge cannot fetch.
    {
        tls::Options tlsOpts;
        tlsOpts.verify = settings.global.tlsVerify;
        tlsOpts.caPath = settings.global.tlsCaPath;
        std::string tlsError;
        if (!tlsConfigure(tlsOpts, tlsError)) {
            log::warn(log::Area::App, "HTTPS direct streaming unavailable: {} (LMS will proxy)",
                      tlsError);
        } else if (tlsUsable()) {
            log::info(log::Area::App, "TLS ready (verify={})", tlsOpts.verify ? "on" : "off");
        }
    }

    DeviceRegistry registry;
    SessionManager manager(settings, persistence);
    registry.setCallback([&manager](DeviceRegistry::Event ev, const AirplayDevice& dev) {
        manager.onRegistryEvent(ev, dev);
    });

    // User-authored sections are static players: they run at startup whether
    // or not discovery is on (a section with a target needs no discovery at
    // all; one without still registers with LMS and waits for a target).
    for (const auto& player : persistence.staticPlayers()) {
        if (!player.enabled) continue;
        AirplayDevice dev;
        dev.id = player.key;
        dev.name = player.name;
        manager.onRegistryEvent(DeviceRegistry::Event::Added, dev);
    }

    std::string error;
    if (settings.global.mdnsDebug) {
        MdnsBrowser browser;
        MdnsBrowser::RecordCallback cb = [](const MdnsRecord& rec, MdnsBrowser::RecordEvent ev) {
            const char* what = (ev == MdnsBrowser::RecordEvent::Added) ? "added" : "removed";
            log::info(log::Area::Mdns, "record {} {} {} -> {}", what, rec.type, rec.instance,
                      rec.port ? rec.host + ":" + std::to_string(rec.port) : std::string());
            for (const auto& [k, v] : rec.txt) log::info(log::Area::Mdns, "  txt {}={}", k, v);
        };
        if (!browser.start(settings.global.mdnsIface, cb, error)) {
            log::error(log::Area::Mdns, "browse failed: {}", error);
            return;
        }
        log::info(log::Area::Mdns, "debug mode: browsing (ctrl-c to exit)");
        while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        browser.stop();
        return;
    }

    if (!settings.global.discovery) {
        log::info(log::Area::App, "discovery disabled; running static devices only");
    }
    MdnsBrowser browser;
    MdnsBrowser::RecordCallback cb = [&registry](const MdnsRecord& rec,
                                                 MdnsBrowser::RecordEvent ev) {
        if (rec.type == "_raop._tcp") {
            if (ev == MdnsBrowser::RecordEvent::Added)
                registry.onRaopAdded(rec.instance, rec.host, rec.port, rec.txt);
            else
                registry.onRaopGone(rec.instance);
        } else if (rec.type == "_airplay._tcp") {
            if (ev == MdnsBrowser::RecordEvent::Added)
                registry.onAirplayAdded(rec.instance, rec.host, rec.port, rec.txt);
            else
                registry.onAirplayGone(rec.instance);
        }
    };
    if (settings.global.discovery) {
        if (!browser.start(settings.global.mdnsIface, cb, error)) {
            log::warn(log::Area::Mdns, "browse failed: {} (continuing without discovery)", error);
        }
    }

    log::info(log::Area::App, "running (ctrl-c to exit)");
    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    browser.stop();
}

}  // namespace squeeze2raop2
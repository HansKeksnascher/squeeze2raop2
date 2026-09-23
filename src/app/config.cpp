#include "app/config.h"

#include "app/version.h"
#include "common/log.h"

#include <cstdio>

namespace squeeze2raop2 {

namespace {

void printHelp() {
    std::printf(
        "squeeze2raop2 - Squeezebox to AirPlay 2 bridge\n"
        "usage: squeeze2raop2 [--config <file>] [-V|--version] [-h|--help]\n\n"
        "All settings live in an INI-style config/state file (default\n"
        "squeeze2raop2.conf in the current directory):\n\n"
        "  [global]\n"
        "    lms = <host[:port]>   connect to this LMS (omit for UDP discovery)\n"
        "    discovery = on|off    spawn sessions for discovered devices\n"
        "    iface = <name>        mdns network interface (default: all)\n"
        "    mdns-debug = on|off   browse-only mDNS debug mode\n"
        "    server-timeout-ms = N  reconnect after N ms of LMS silence (1000-600000)\n"
        "    log = off|error|warn|info|debug\n"
        "    auto-register = on|off  create sections for discovered devices\n\n"
        "  [default]               defaults inherited by every [player]\n"
        "    protocol = ap2|ap1    target platform (default ap2)\n"
        "    password = <pw>       RTSP digest password for pw=true receivers\n"
        "    enabled = on|off\n"
        "    volume = lms|fixed    follow the LMS slider or play at volume-pct\n"
        "    volume-map = \"<db:pct, ...>\"  LMS slider percent -> AirPlay dBFS\n"
        "    volume-pct = <N>      AirPlay volume percent 0.5-100\n"
        "    latency-ms = <N>      scheduled AirPlay latency 250-2000 ms\n"
        "    sink = <file>         dump stream PCM to file\n"
        "    pace = realtime|fast  pace sink consumption to play time\n\n"
        "  [player \"Name\"]         one player; identity/target:\n"
        "    id = <12-hex>         match key (mDNS device id)\n"
        "    mac = xx:..:xx        virtual MAC override\n"
        "    target = <host[:port]>  fixed AirPlay receiver (static player)\n"
        "    plus any [default] key to override it\n\n"
        "  -h --help               this text\n"
        "  -V --version            print version and exit\n");
}

}  // namespace

ResolvedPlayerConfig resolvePlayer(const PlayerConfig& defaults, const PlayerConfig& player) {
    ResolvedPlayerConfig r;

    // Identity/target: player only.
    r.id = player.id.value_or(std::string());
    r.name = player.name.value_or(std::string());
    r.key = !r.id.empty() ? r.id : r.name;
    if (player.mac) {
        r.mac = *player.mac;
        r.explicitMac = true;
    }
    if (player.targetHost && !player.targetHost->empty()) {
        uint16_t port = player.targetPort.value_or(7000);
        r.target = std::make_pair(*player.targetHost, port);
    }
    r.autoRegistered = player.autoSection.value_or(false);

    auto pick = [&](const auto& opt, const auto& fallback, auto dflt) {
        if (opt) return *opt;
        if (fallback) return *fallback;
        return dflt;
    };

    r.enabled = pick(player.enabled, defaults.enabled, true);
    r.airplay2 = pick(player.airplay2, defaults.airplay2, true);
    r.password = pick(player.password, defaults.password, std::string());
    r.volumeMode = pick(player.volumeMode, defaults.volumeMode, VolumeMode::Lms);
    r.volumeMap = pick(player.volumeMap, defaults.volumeMap, std::string(kDefaultVolumeMap));
    r.volPct = pick(player.volPct, defaults.volPct, 0.7f);
    r.latencyMs = pick(player.latencyMs, defaults.latencyMs, 500);
    r.paceRealtime = pick(player.paceRealtime, defaults.paceRealtime, true);
    r.sinkPath = player.sinkPath ? player.sinkPath : defaults.sinkPath;

    return r;
}

std::optional<Args> parseArgs(int argc, char** argv, int& exitCode) {
    Args args;
    exitCode = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printHelp();
            return std::nullopt;
        }
        if (arg == "-V" || arg == "--version") {
            std::printf("squeeze2raop2 %s\n", SQUEEZE2RAOP2_VERSION);
            return std::nullopt;
        }
        if (arg == "--config") {
            if (i + 1 >= argc) {
                log::error("--config requires a file path");
                exitCode = 1;
                return std::nullopt;
            }
            args.configPath = argv[++i];
            continue;
        }
        log::error(
            "unknown option {} (behavior is configured in the config file; "
            "use --config <file>)",
            arg);
        exitCode = 1;
        return std::nullopt;
    }
    return args;
}

}  // namespace squeeze2raop2
#include "config.h"

#include "log.h"
#include "util.h"

#include <charconv>
#include <optional>

namespace squeeze2raop2 {

namespace {

bool requireValue(const std::string& flag, const char* value, std::string& out) {
    if (!value) {
        log::error("flag {} requires a value", flag);
        return false;
    }
    out = value;
    return true;
} // namespace

std::optional<uint16_t> parsePort(const std::string& text) {
    unsigned port = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (ec != std::errc{} || ptr != text.data() + text.size() || port < 1 || port > 65535)
        return std::nullopt;
    return static_cast<uint16_t>(port);
} // namespace squeeze2raop2

} // namespace

std::optional<Settings> parseCommandLine(int argc, char** argv, int& exitCode) {
    Settings s;
    PlayerSettings player;
    player.name = "AirPlay";

    exitCode = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };

        if (arg == "-h" || arg == "--help") {
            std::printf(
                "squeeze2raop2 - Squeezebox to AirPlay 2 bridge\n"
                "usage: squeeze2raop2 [options]\n\n"
                "  --lms <host[:port]>   connect to this LMS (default: UDP discovery on 3483)\n"
                "  --name <name>         player name (default: AirPlay)\n"
                "  --mac <xx:..>         player MAC override\n"
                "  --sink <file>         dump stream PCM to file (M1 test sink)\n"
                "  --pace realtime|fast  pace sink consumption to play time (default realtime)\n"
                "  --ap <host[:port]>    AirPlay receiver target (default port 7000)\n"
                "  --ap-protocol ap2|ap1  AirPlay2 native or classic RAOP\n"
                "  --ap-password <pw>    RTSP digest password for pw=true receivers\n"
                "  --device <NAME>       also register this name as static player\n"
                "                        (used when discovery is unavailable)\n"
                "  --state <file>        persistent MAC/credential store (default squeeze2raop2.state)\n"
                "  --iface <name>        mdns network interface (default: all)\n"
                "  --mdns-debug          browse-only mDNS debug mode (no LMS/AirPlay sessions)\n"
                "  --discovery on|off    spawn sessions for discovered devices (default on)\n"
                "  --vol-mode lms|fixed  follow the LMS slider via AUDG (default; LMS"
                "\n"
                "                        minimum = receiver mute, maximum = 0 dB) or\n"
                "                        ignore LMS volume and play at --vol-pct\n"
                "  --vol-pct <N>         AirPlay volume percent 0.5-100 (default 0.7,\n"
                "                        about -29.8 dB; fixed-mode level and the\n"
                "                        fallback until LMS pushes the slider)\n"
                "  --ap-latency-ms <N>   scheduled AirPlay latency 250-2000 ms (default 500;\n"
                "                        lower = snappier but more dropout-prone)\n"
                "  --log <level>         off|error|warn|info|debug\n"
                "  -h --help             this text\n");
            return std::nullopt;
        }

        std::string v;
        if (arg == "--lms") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            auto colon = v.find(':');
            if (colon != std::string::npos) {
                auto port = parsePort(v.substr(colon + 1));
                if (!port) {
                    log::error("--lms requires a numeric port 1-65535, got '{}'",
                               v.substr(colon + 1));
                    return std::nullopt;
                }
                s.lmsHost = v.substr(0, colon);
                s.lmsPort = *port;
            } else {
                s.lmsHost = v;
            }
        } else if (arg == "--name") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            player.name = v;
            player.explicitName = true;
        } else if (arg == "--mac") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            if (!macFromString(v, player.mac)) {
                log::error("invalid MAC '{}'", v);
                return std::nullopt;
            }
            player.explicitMac = true;
        } else if (arg == "--sink") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            s.sinkPath = v;
        } else if (arg == "--pace") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            if (v == "fast") s.paceRealtime = false;
            else if (v == "realtime") s.paceRealtime = true;
            else { log::error("--pace must be realtime|fast"); return std::nullopt; }
        } else if (arg == "--ap") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            s.ap.enabled = true;
            auto colon = v.find(':');
            if (colon != std::string::npos) {
                auto port = parsePort(v.substr(colon + 1));
                if (!port) {
                    log::error("--ap requires a numeric port 1-65535, got '{}'",
                               v.substr(colon + 1));
                    return std::nullopt;
                }
                s.ap.host = v.substr(0, colon);
                s.ap.port = *port;
            } else {
                s.ap.host = v;
            }
        } else if (arg == "--ap-protocol") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            if (v == "ap1") s.ap.airplay2 = false;
            else if (v == "ap2") s.ap.airplay2 = true;
            else { log::error("--ap-protocol must be ap1|ap2"); return std::nullopt; }
        } else if (arg == "--ap-password") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            s.ap.password = v;
        } else if (arg == "--device") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            s.staticDevices.emplace_back(v, v);
        } else if (arg == "--state") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            s.statePath = v;
        } else if (arg == "--iface") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            s.mdnsIface = v;
        } else if (arg == "--mdns-debug") {
            s.mdnsDebug = true;
        } else if (arg == "--discovery") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            if (v == "on") s.discovery = true;
            else if (v == "off") s.discovery = false;
            else { log::error("--discovery must be on|off"); return std::nullopt; }
        } else if (arg == "--vol-mode") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            if (v == "lms") s.volumeMode = VolumeMode::Lms;
            else if (v == "fixed") s.volumeMode = VolumeMode::Fixed;
            else { log::error("--vol-mode must be lms|fixed"); return std::nullopt; }
        } else if (arg == "--vol-pct") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            try {
                s.volPct = std::stof(v);
            } catch (const std::exception&) {
                log::error("--vol-pct must be a number, got '{}'", v);
                return std::nullopt;
            }
            if (s.volPct < 0.5f || s.volPct > 100.f) {
                log::error("--vol-pct must be 0.5-100 (0 would be mute)");
                return std::nullopt;
            }
        } else if (arg == "--ap-latency-ms") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            try {
                s.apLatencyMs = std::stoi(v);
            } catch (const std::exception&) {
                log::error("--ap-latency-ms must be a number, got '{}'", v);
                return std::nullopt;
            }
            if (s.apLatencyMs < 250 || s.apLatencyMs > 2000) {
                log::error("--ap-latency-ms must be 250-2000 "
                           "(receiver latencyMin..Max)");
                return std::nullopt;
            }
        } else if (arg == "--log") {
            if (!requireValue(arg, value(), v)) return std::nullopt;
            if (v == "off") s.logLevel = log::Level::Off;
            else if (v == "error") s.logLevel = log::Level::Error;
            else if (v == "warn") s.logLevel = log::Level::Warn;
            else if (v == "info") s.logLevel = log::Level::Info;
            else if (v == "debug") s.logLevel = log::Level::Debug;
            else { log::error("--log must be off|error|warn|info|debug"); return std::nullopt; }
        } else {
            log::error("unknown option {}", arg);
            return std::nullopt;
        }
    }

    player.deviceId = player.name;
    if (!player.explicitMac) player.mac = fakeMacFor(player.deviceId);
    s.players.push_back(player);
    return s;
}

}

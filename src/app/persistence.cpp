#include "app/persistence.h"

#include "common/log.h"
#include "common/util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

namespace squeeze2raop2 {

namespace {

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseBool(std::string_view v, bool& out) {
    const std::string s = lower(std::string(trimView(v)));
    if (s == "on" || s == "true" || s == "yes" || s == "1") {
        out = true;
        return true;
    }
    if (s == "off" || s == "false" || s == "no" || s == "0") {
        out = false;
        return true;
    }
    return false;
}

// Strict decimal integer parse; false on malformed input or trailing junk.
template <typename T>
bool parseNumber(std::string_view s, T& out) {
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

std::optional<uint16_t> parsePort(std::string_view s) {
    uint16_t p = 0;
    if (!parseNumber(s, p) || p == 0) return std::nullopt;  // from_chars rejects > 65535
    return p;
}

std::string unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        std::string out;
        out.reserve(s.size() - 2);
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size())
                out.push_back(s[++i]);
            else
                out.push_back(s[i]);
        }
        return out;
    }
    return std::string(s);
}

bool isHex12(std::string_view s) {
    if (s.size() != 12) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

// "[global]" / "[default]" / "[player \"Name\"]" -> canonical section name.
bool parseSectionHeader(std::string_view raw, std::string& canonical, bool& isPlayer) {
    const std::string_view t = trimView(raw);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
    const std::string_view inner = trimView(t.substr(1, t.size() - 2));
    if (inner == "global") {
        canonical = "global";
        isPlayer = false;
        return true;
    }
    if (inner == "default") {
        canonical = "default";
        isPlayer = false;
        return true;
    }
    if (inner.size() >= 6 && lower(std::string(inner.substr(0, 6))) == "player") {
        const std::string_view rest = trimView(inner.substr(6));
        if (rest.empty()) return false;
        canonical = unquote(rest);
        isPlayer = true;
        return true;
    }
    return false;
}

std::string legacyStatePath(const std::string& configPath) {
    const size_t slash = configPath.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? "" : configPath.substr(0, slash + 1);
    std::string stem = (slash == std::string::npos) ? configPath : configPath.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    return dir + stem + ".state";
}

}  // namespace

// --- matching / state -------------------------------------------------------

const Persistence::Section* Persistence::findSection(const std::string& key) const {
    const std::string lk = lower(key);
    for (const auto& s : sections_)
        if (!s.config.id.value_or("").empty() && lower(*s.config.id) == lk) return &s;
    for (const auto& s : sections_)
        if (lower(s.name) == lk) return &s;
    return nullptr;
}

Persistence::Section* Persistence::findSection(const std::string& key) {
    const Persistence& self = *this;
    return const_cast<Section*>(self.findSection(key));
}

Persistence::Section& Persistence::ensureSection(const std::string& key, const std::string& name,
                                                 bool autoRegistered) {
    if (Section* existing = findSection(key)) return *existing;
    Section s;
    s.name = name.empty() ? key : name;
    s.config.name = s.name;
    s.autoRegistered = autoRegistered;
    sections_.push_back(std::move(s));
    return sections_.back();
}

void Persistence::assignMacLocked(Section& s) {
    if (s.hasMac) return;
    std::array<uint8_t, 6> candidate = fakeMacFor(s.config.id.value_or(s.name));
    for (int tries = 0; tries < 256; ++tries) {
        bool conflict = false;
        for (const auto& other : sections_) {
            if (&other != &s && other.hasMac && other.mac == candidate) {
                conflict = true;
                break;
            }
        }
        if (!conflict) break;
        candidate[5] = static_cast<uint8_t>((candidate[5] + 1) & 0xFF);
    }
    s.mac = candidate;
    s.hasMac = true;
    s.config.mac = candidate;
    dirty_ = true;
}

ResolvedPlayerConfig Persistence::finalizeSection(Section& s) const {
    ResolvedPlayerConfig r = resolvePlayer(defaults_, s.config);
    r.mac = s.mac;
    r.explicitMac = s.config.mac.has_value();
    r.key = !s.config.id.value_or("").empty() ? lower(*s.config.id) : s.name;
    r.autoRegistered = s.autoRegistered;
    return r;
}

std::vector<ResolvedPlayerConfig> Persistence::staticPlayers() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ResolvedPlayerConfig> out;
    for (Section& s : sections_) {
        if (s.autoRegistered) continue;
        assignMacLocked(s);
        out.push_back(finalizeSection(s));
    }
    flushIfDirtyLocked();
    return out;
}

std::optional<std::string> Persistence::credsFor(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Section* s = findSection(key);
    if (!s || s->creds.empty()) return std::nullopt;
    return s->creds;
}

void Persistence::saveCreds(const std::string& key, const std::string& credsJson) {
    std::lock_guard<std::mutex> lock(mutex_);
    Section* s = findSection(key);
    if (!s) return;
    s->creds = credsJson;
    saveLocked();
}

void Persistence::savePlayerName(const std::string& key, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    Section* s = findSection(key);
    if (!s || name.empty() || s->config.name == name) return;
    s->config.name = name;
    s->hasNameOverride = true;
    saveLocked();
}

std::optional<ResolvedPlayerConfig> Persistence::resolve(const std::string& deviceId,
                                                         const std::string& deviceName,
                                                         bool autoRegister) {
    std::lock_guard<std::mutex> lock(mutex_);
    Section* match = nullptr;

    // 1. explicit id match (case-insensitive 12-hex).
    const std::string id = lower(deviceId);
    if (!deviceId.empty()) {
        for (auto& s : sections_)
            if (s.config.id && lower(*s.config.id) == id) {
                match = &s;
                break;
            }
    }
    // 2. virtual-MAC match: a section carrying the MAC this id would get.
    if (!match && isHex12(deviceId)) {
        const auto candidate = fakeMacFor(deviceId);
        for (auto& s : sections_)
            if (s.hasMac && s.mac == candidate) {
                match = &s;
                break;
            }
    }
    // 3. name match.
    if (!match && !deviceName.empty()) {
        for (auto& s : sections_)
            if (lower(s.name) == lower(deviceName)) {
                match = &s;
                break;
            }
    }

    if (!match) {
        if (!autoRegister) return std::nullopt;
        const std::string key = deviceId.empty() ? deviceName : id;
        const std::string name = deviceName.empty() ? key : deviceName;
        Section& created = ensureSection(key, name, /*autoRegistered=*/true);
        if (isHex12(key)) created.config.id = id;
        created.config.name = created.name;
        assignMacLocked(created);
        flushIfDirtyLocked();
        return finalizeSection(created);
    }

    assignMacLocked(*match);
    // Display the receiver's friendly mDNS name (LMS shows it instead of the
    // device id). A 'setd' rename persisted as the machine-managed 'name' key
    // is an explicit override and wins.
    if (!match->hasNameOverride && !deviceName.empty()) match->config.name = deviceName;
    flushIfDirtyLocked();
    return finalizeSection(*match);
}

// --- parsing ----------------------------------------------------------------

bool Persistence::fail(std::string& error, int lineNo, std::string_view msg) const {
    error = "config " + path_ + ":" + std::to_string(lineNo) + ": " + std::string(msg);
    return false;
}

bool Persistence::parseGlobalKey(std::string_view key, std::string_view value, int lineNo,
                                 std::string& error) {
    bool b = false;
    if (key == "lms") {
        const auto colon = value.find(':');
        if (colon != std::string_view::npos) {
            const auto port = parsePort(value.substr(colon + 1));
            if (!port) return fail(error, lineNo, "lms port must be 1-65535");
            global_.lmsHost = std::string(value.substr(0, colon));
            global_.lmsPort = *port;
        } else {
            global_.lmsHost = std::string(value);
        }
    } else if (key == "discovery") {
        if (!parseBool(value, b)) return fail(error, lineNo, "discovery must be on|off");
        global_.discovery = b;
    } else if (key == "iface") {
        global_.mdnsIface = std::string(value);
    } else if (key == "mdns-debug") {
        if (!parseBool(value, b)) return fail(error, lineNo, "mdns-debug must be on|off");
        global_.mdnsDebug = b;
    } else if (key == "auto-register") {
        if (!parseBool(value, b)) return fail(error, lineNo, "auto-register must be on|off");
        global_.autoRegister = b;
    } else if (key == "server-timeout-ms") {
        unsigned v = 0;
        if (!parseNumber(value, v))
            return fail(error, lineNo, "server-timeout-ms must be a number");
        if (v < 1000 || v > 600000)
            return fail(error, lineNo, "server-timeout-ms must be 1000-600000");
        global_.serverTimeoutMs = v;
    } else if (key == "log") {
        if (value == "off")
            global_.logLevel = log::Level::Off;
        else if (value == "error")
            global_.logLevel = log::Level::Error;
        else if (value == "warn")
            global_.logLevel = log::Level::Warn;
        else if (value == "info")
            global_.logLevel = log::Level::Info;
        else if (value == "debug")
            global_.logLevel = log::Level::Debug;
        else
            return fail(error, lineNo, "log must be off|error|warn|info|debug");
    } else {
        log::warn(log::Area::App, "config {}:{}: unknown [global] key '{}'", path_, lineNo, key);
    }
    return true;
}

bool Persistence::parsePlayerKey(std::string_view key, std::string_view value, int lineNo,
                                 PlayerConfig& pc, Section* section, bool isDefault,
                                 std::string& error) {
    // Identity/machine keys are meaningless in [default]; reject them up front.
    if (isDefault &&
        (key == "id" || key == "mac" || key == "name" || key == "target" || key == "creds")) {
        log::warn(log::Area::App, "config {}:{}: '{}' is not valid in [default], ignoring", path_,
                  lineNo, key);
        return true;
    }

    if (key == "id") {
        pc.id = std::string(value);
    } else if (key == "mac") {
        std::array<uint8_t, 6> mac{};
        if (!macFromString(value, mac))
            return fail(error, lineNo, "invalid mac '" + std::string(value) + "'");
        pc.mac = mac;
        section->mac = mac;
        section->hasMac = true;
    } else if (key == "name") {
        pc.name = std::string(value);
        section->hasNameOverride = true;
    } else if (key == "target") {
        const auto colon = value.find(':');
        if (colon != std::string_view::npos) {
            const auto port = parsePort(value.substr(colon + 1));
            if (!port) return fail(error, lineNo, "target port must be 1-65535");
            pc.targetHost = std::string(value.substr(0, colon));
            pc.targetPort = *port;
        } else {
            pc.targetHost = std::string(value);
        }
    } else if (key == "protocol") {
        if (value == "ap1")
            pc.airplay2 = false;
        else if (value == "ap2")
            pc.airplay2 = true;
        else
            return fail(error, lineNo, "protocol must be ap1|ap2");
    } else if (key == "password") {
        pc.password = std::string(value);
    } else if (key == "enabled") {
        bool b = false;
        if (!parseBool(value, b)) return fail(error, lineNo, "enabled must be on|off");
        pc.enabled = b;
    } else if (key == "volume") {
        if (value == "lms")
            pc.volumeMode = VolumeMode::Lms;
        else if (value == "fixed")
            pc.volumeMode = VolumeMode::Fixed;
        else
            return fail(error, lineNo, "volume must be lms|fixed");
    } else if (key == "volume-map") {
        if (!VolumeAnchors::parse(value))
            return fail(error, lineNo,
                        "volume-map needs \"db:pct, ...\" pairs, ascending pct 1-100, db <= 0");
        pc.volumeMap = std::string(value);
    } else if (key == "volume-pct") {
        float parsed = 0.0f;
        if (!parseNumber(value, parsed)) return fail(error, lineNo, "volume-pct must be a number");
        if (parsed < 0.5f || parsed > 100.f)
            return fail(error, lineNo, "volume-pct must be 0.5-100 (0 would be mute)");
        pc.volPct = parsed;
    } else if (key == "latency-ms") {
        int parsed = 0;
        if (!parseNumber(value, parsed)) return fail(error, lineNo, "latency-ms must be a number");
        if (parsed < 250 || parsed > 2000)
            return fail(error, lineNo, "latency-ms must be 250-2000 (receiver latencyMin..Max)");
        pc.latencyMs = parsed;
    } else if (key == "sink") {
        pc.sinkPath = std::string(value);
    } else if (key == "pace") {
        if (value == "fast")
            pc.paceRealtime = false;
        else if (value == "realtime")
            pc.paceRealtime = true;
        else
            return fail(error, lineNo, "pace must be realtime|fast");
    } else if (key == "creds") {
        section->creds = std::string(value);
    } else if (key == "auto") {
        bool b = false;
        if (!parseBool(value, b)) return fail(error, lineNo, "auto must be on|off");
        pc.autoSection = b;
        if (!isDefault && b) section->autoRegistered = true;
    } else {
        log::warn(log::Area::App, "config {}:{}: unknown key '{}'", path_, lineNo, key);
    }
    return true;
}

bool Persistence::parse(const std::string& text, Settings& out, std::string& error) {
    lines_.clear();
    sections_.clear();
    defaults_ = PlayerConfig{};
    global_ = GlobalConfig{};

    enum class Kind : std::uint8_t { None, Global, Default, Player, Unknown };
    Kind current = Kind::None;
    std::string currentName;
    Section* currentSection = nullptr;

    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        Line line;
        line.raw = raw;
        const std::string_view trimmed = trimView(raw);
        if (trimmed.empty()) {
            line.kind = Line::Kind::Blank;
            lines_.push_back(std::move(line));
            continue;
        }
        if (trimmed.front() == '#' || trimmed.front() == ';') {
            line.kind = Line::Kind::Comment;
            lines_.push_back(std::move(line));
            continue;
        }
        if (trimmed.front() == '[') {
            std::string canonical;
            bool isPlayer = false;
            if (!parseSectionHeader(raw, canonical, isPlayer)) {
                log::warn(log::Area::App, "config {}:{}: unknown section header, ignoring", path_,
                          lineNo);
                line.kind = Line::Kind::Other;
                lines_.push_back(std::move(line));
                current = Kind::Unknown;
                currentSection = nullptr;
                continue;
            }
            line.kind = Line::Kind::Section;
            line.section = canonical;
            currentName = canonical;
            if (canonical == "global") {
                current = Kind::Global;
                currentSection = nullptr;
            } else if (canonical == "default") {
                current = Kind::Default;
                currentSection = nullptr;
            } else {
                current = Kind::Player;
                Section& s = ensureSection(canonical, canonical, false);
                s.config.name = canonical;
                currentSection = &s;
            }
            lines_.push_back(std::move(line));
            continue;
        }

        const size_t eq = raw.find('=');
        if (eq == std::string::npos) {
            log::warn(log::Area::App, "config {}:{}: not a key=value line, ignoring", path_,
                      lineNo);
            line.kind = Line::Kind::Other;
            lines_.push_back(std::move(line));
            continue;
        }
        const std::string keyRaw =
            lower(std::string(trimView(std::string_view(raw).substr(0, eq))));
        const std::string valueText = unquote(trimView(std::string_view(raw).substr(eq + 1)));
        line.kind = Line::Kind::Key;
        line.section = currentName;
        line.key = keyRaw;
        line.value = valueText;
        const size_t vstart = raw.find_first_not_of(" \t", eq + 1);
        line.valuePrefix = raw.substr(0, (vstart == std::string::npos) ? raw.size() : vstart);
        if (vstart == std::string::npos) line.valuePrefix.push_back(' ');
        if (current == Kind::None || current == Kind::Unknown) {
            log::warn(log::Area::App, "config {}:{}: key '{}' outside a section, ignoring", path_,
                      lineNo, keyRaw);
            lines_.push_back(std::move(line));
            continue;
        }

        if (current == Kind::Global) {
            if (!parseGlobalKey(keyRaw, valueText, lineNo, error)) return false;
            lines_.push_back(std::move(line));
            continue;
        }

        // [default] / [player]
        const bool isDefault = current == Kind::Default;
        PlayerConfig& pc = isDefault ? defaults_ : currentSection->config;
        if (!parsePlayerKey(keyRaw, valueText, lineNo, pc, currentSection, isDefault, error))
            return false;
        lines_.push_back(std::move(line));
    }

    // Resolve every section: static (user) and auto alike.
    out.players.clear();
    out.defaults = defaults_;
    for (Section& s : sections_) {
        assignMacLocked(s);
        out.players.push_back(finalizeSection(s));
    }
    out.global = global_;
    out.global.configPath = path_;
    return true;
}

// --- loading / saving -------------------------------------------------------

bool Persistence::importLegacy(const std::string& legacyPath, std::string& error) {
    std::ifstream in(legacyPath);
    if (!in) return false;
    std::string raw;
    int lineNo = 0;
    int imported = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string_view t = trimView(raw);
        if (t.empty() || t.front() == '#') continue;
        std::istringstream iss{std::string(t)};
        std::string tag, id;
        if (!(iss >> tag >> id)) continue;
        const std::string key = urlDecode(id);
        const bool hex = isHex12(key);
        const std::string canonical = hex ? lower(key) : key;
        if (tag == "mac") {
            std::string macText;
            iss >> macText;
            std::array<uint8_t, 6> mac{};
            if (!macFromString(macText, mac)) {
                log::warn(log::Area::App, "legacy state {}:{}: bad mac, skipping", legacyPath,
                          lineNo);
                continue;
            }
            Section& s = ensureSection(canonical, canonical, hex);
            if (hex) s.config.id = canonical;
            s.config.name = s.name;
            s.mac = mac;
            s.hasMac = true;
            ++imported;
        } else if (tag == "creds") {
            std::string rest;
            std::getline(iss, rest);
            const size_t start = rest.find_first_not_of(' ');
            if (start == std::string::npos) continue;
            Section& s = ensureSection(canonical, canonical, hex);
            if (hex) s.config.id = canonical;
            s.config.name = s.name;
            s.creds = rest.substr(start);
            ++imported;
        }
    }
    (void)error;
    if (imported > 0)
        log::info(log::Area::App, "imported {} entries from {}", imported, legacyPath);
    return imported > 0;
}

void Persistence::writeTemplate() {
    std::ofstream out(path_, std::ios::trunc);
    if (!out) return;
    out << "# squeeze2raop2 config + state. Edit [global]/[default]/[player]; the\n"
           "# program only rewrites the machine-managed 'mac', 'creds' and 'name'\n"
           "# keys.\n"
           "\n"
           "[global]\n"
           "# lms = 192.168.1.10:3483   (omit for UDP discovery on 3483)\n"
           "discovery = on\n"
           "# iface = eth0\n"
           "# mdns-debug = off\n"
           "# server-timeout-ms = 35000\n"
           "log = info\n"
           "auto-register = on\n"
           "\n"
           "[default]\n"
           "protocol = ap2\n"
           "# password =\n"
           "enabled = on\n"
           "volume = lms\n"
           "volume-map = -30:1, -23:16, -15:50, 0:100\n"
           "volume-pct = 0.7\n"
           "latency-ms = 500\n"
           "# sink =\n"
           "pace = realtime\n";
    out.flush();
}

bool Persistence::open(const std::string& path, Settings& out, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t pos = path.find_last_of('/');
        if (pos != std::string::npos) (void)mkdir(path.substr(0, pos).c_str(), 0755);
        path_ = path;
    }

    std::ifstream probe(path_);
    if (!probe.good()) {
        probe.close();
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string legacy = legacyStatePath(path_);
        bool imported = false;
        if (legacy != path_) imported = importLegacy(legacy, error);
        if (!imported) {
            writeTemplate();
        } else {
            saveLocked();
        }
    }

    std::ifstream in(path_);
    if (!in) {
        error = "cannot open config file " + path_;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::lock_guard<std::mutex> lock(mutex_);
    const bool ok = parse(text, out, error);
    if (ok) flushIfDirtyLocked();
    return ok;
}

void Persistence::flushIfDirtyLocked() {
    if (!dirty_) return;
    saveLocked();
    dirty_ = false;
}

bool Persistence::saveLocked() {
    std::set<std::string> sectionNames;
    for (const auto& s : sections_) sectionNames.insert(s.name);

    std::set<std::string> headerSeen;
    std::set<std::string> hasMacLine, hasCredsLine, hasAutoLine, hasNameLine;
    for (const auto& l : lines_) {
        if (l.kind == Line::Kind::Section) headerSeen.insert(l.section);
        if (l.kind == Line::Kind::Key) {
            if (l.key == "mac") hasMacLine.insert(l.section);
            if (l.key == "creds") hasCredsLine.insert(l.section);
            if (l.key == "auto") hasAutoLine.insert(l.section);
            if (l.key == "name") hasNameLine.insert(l.section);
        }
    }

    auto sectionByName = [this](const std::string& name) -> const Section* {
        for (const auto& sec : sections_)
            if (sec.name == name) return &sec;
        return nullptr;
    };

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            log::error(log::Area::App, "cannot write config file {}", tmp);
            return false;
        }
        for (const auto& l : lines_) {
            if (l.kind == Line::Kind::Key &&
                (l.key == "mac" || l.key == "creds" || l.key == "name")) {
                const Section* s = sectionByName(l.section);
                if (!s) {
                    out << l.raw << "\n";
                    continue;
                }
                if (l.key == "mac") {
                    if (s->hasMac) out << l.valuePrefix << macToString(s->mac) << "\n";
                } else if (l.key == "creds") {
                    if (!s->creds.empty()) out << l.valuePrefix << oneLine(s->creds) << "\n";
                } else if (s->hasNameOverride) {
                    out << l.valuePrefix << s->config.name.value_or(s->name) << "\n";
                }
                continue;
            }
            out << l.raw << "\n";
            if (l.kind == Line::Kind::Section) {
                const Section* s = sectionByName(l.section);
                if (!s) continue;
                if (s->hasMac && !hasMacLine.count(l.section))
                    out << "mac = " << macToString(s->mac) << "\n";
                if (!s->creds.empty() && !hasCredsLine.count(l.section))
                    out << "creds = " << oneLine(s->creds) << "\n";
                if (s->hasNameOverride && !hasNameLine.count(l.section))
                    out << "name = " << s->config.name.value_or(s->name) << "\n";
                if (s->autoRegistered && !hasAutoLine.count(l.section)) out << "auto = true\n";
            }
        }
        // New auto-created sections have no header in lines_ yet.
        for (const auto& s : sections_) {
            if (headerSeen.count(s.name)) continue;
            out << "\n[player \"" << s.name << "\"]\n";
            if (s.config.id && !s.config.id->empty()) out << "id = " << *s.config.id << "\n";
            if (s.hasMac) out << "mac = " << macToString(s.mac) << "\n";
            if (!s.creds.empty()) out << "creds = " << oneLine(s.creds) << "\n";
            if (s.hasNameOverride) out << "name = " << s.config.name.value_or(s.name) << "\n";
            if (s.autoRegistered) out << "auto = true\n";
        }
        out.flush();
        if (!out) {
            log::error(log::Area::App, "cannot write config file {}", tmp);
            out.close();
            ::remove(tmp.c_str());
            return false;
        }
    }
    if (const int fd = ::open(tmp.c_str(), O_RDONLY); fd >= 0) {
        (void)::fsync(fd);
        (void)::close(fd);
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        log::error(log::Area::App, "cannot replace config file {}: {}", path_, errnoMessage(errno));
        ::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace squeeze2raop2
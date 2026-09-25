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

bool parseBool(std::string_view v, bool& out) {
    const std::string s = toLower(trimView(v));
    for (const char* t : kTrueValues) {
        if (s == t) {
            out = true;
            return true;
        }
    }
    for (const char* f : kFalseValues) {
        if (s == f) {
            out = false;
            return true;
        }
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

// One key=value parse: validates and stores, or records the uniform
// "config <path>:<line>: <key> must be ..." error. Centralizes the
// parse+range+message shape the global/player readers used to repeat.
struct KeySetter {
    const std::string& path;
    int lineNo;
    std::string& error;

    bool fail(std::string_view msg) const {
        error = "config " + path + ":" + std::to_string(lineNo) + ": " + std::string(msg);
        return false;
    }

    bool boolKey(std::string_view value, bool& out, std::string_view key) const {
        if (!parseBool(value, out)) return fail(std::string(key) + " must be on|off");
        return true;
    }

    template <typename T>
    bool numberKey(std::string_view value, T& out, T lo, T hi, std::string_view key,
                   std::string_view expect) const {
        T parsed{};
        if (!parseNumber(value, parsed)) return fail(std::string(key) + " must be a number");
        if (parsed < lo || parsed > hi)
            return fail(std::string(key) + " must be " + std::string(expect));
        out = parsed;
        return true;
    }

    bool macKey(std::string_view value, std::array<uint8_t, 6>& out) const {
        if (!macFromString(value, out)) return fail("invalid mac '" + std::string(value) + "'");
        return true;
    }
};

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
    if (inner == kSectionGlobal) {
        canonical = kSectionGlobal;
        isPlayer = false;
        return true;
    }
    if (inner == kSectionDefault) {
        canonical = kSectionDefault;
        isPlayer = false;
        return true;
    }
    if (inner.size() >= std::string_view{kSectionPlayer}.size() &&
        toLower(inner.substr(0, std::string_view{kSectionPlayer}.size())) == kSectionPlayer) {
        const std::string_view rest =
            trimView(inner.substr(std::string_view{kSectionPlayer}.size()));
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
    return dir + stem + kLegacyStateSuffix;
}

}  // namespace

// --- matching / state -------------------------------------------------------

const Persistence::Section* Persistence::findSection(const std::string& key) const {
    const std::string lk = toLower(key);
    for (const auto& s : sections_)
        if (!s.config.id.value_or("").empty() && toLower(*s.config.id) == lk) return &s;
    for (const auto& s : sections_)
        if (toLower(s.name) == lk) return &s;
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
    r.key = !s.config.id.value_or("").empty() ? toLower(*s.config.id) : s.name;
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
    const std::string id = toLower(deviceId);
    if (!deviceId.empty()) {
        for (auto& s : sections_)
            if (s.config.id && toLower(*s.config.id) == id) {
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
            if (toLower(s.name) == toLower(deviceName)) {
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

bool Persistence::parseGlobalKey(std::string_view key, std::string_view value, int lineNo,
                                 std::string& error) {
    const KeySetter set{path_, lineNo, error};
    bool b = false;
    if (key == kKeyLms) {
        const auto colon = value.find(':');
        if (colon != std::string_view::npos) {
            const auto port = parsePort(value.substr(colon + 1));
            if (!port) return set.fail("lms port must be 1-65535");
            global_.lmsHost = std::string(value.substr(0, colon));
            global_.lmsPort = *port;
        } else {
            global_.lmsHost = std::string(value);
        }
    } else if (key == kKeyDiscovery) {
        if (!set.boolKey(value, b, "discovery")) return false;
        global_.discovery = b;
    } else if (key == kKeyIface) {
        global_.mdnsIface = std::string(value);
    } else if (key == kKeyMdnsDebug) {
        if (!set.boolKey(value, b, "mdns-debug")) return false;
        global_.mdnsDebug = b;
    } else if (key == kKeyAutoRegister) {
        if (!set.boolKey(value, b, "auto-register")) return false;
        global_.autoRegister = b;
    } else if (key == kKeyVolumeFeedback) {
        if (!set.boolKey(value, b, "volume-feedback")) return false;
        global_.volumeFeedback = b;
    } else if (key == kKeyServerTimeoutMs) {
        if (!set.numberKey(value, global_.serverTimeoutMs, kServerTimeoutMinMs, kServerTimeoutMaxMs,
                           kKeyServerTimeoutMs, "1000-600000"))
            return false;
    } else if (key == kKeySourceTimeoutMs) {
        unsigned v = 0;
        if (!parseNumber(value, v)) return set.fail("source-timeout-ms must be a number");
        if (v != 0 && (v < kSourceTimeoutMinMs || v > kSourceTimeoutMaxMs))
            return set.fail("source-timeout-ms must be 0 (off) or 1000-600000");
        global_.sourceTimeoutMs = v;
    } else if (key == kKeyTlsVerify) {
        if (!set.boolKey(value, b, "tls-verify")) return false;
        global_.tlsVerify = b;
    } else if (key == kKeyTlsCa) {
        global_.tlsCaPath = std::string(value);
    } else if (key == kKeyLog) {
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
            return set.fail("log must be off|error|warn|info|debug");
    } else {
        log::warn(log::Area::App, "config {}:{}: unknown [global] key '{}'", path_, lineNo, key);
    }
    return true;
}

bool Persistence::parsePlayerKey(std::string_view key, std::string_view value, int lineNo,
                                 PlayerConfig& pc, Section* section, bool isDefault,
                                 std::string& error) {
    // Identity/machine keys are meaningless in [default]; reject them up front.
    if (isDefault && (key == kKeyId || key == kKeyMac || key == kKeyName || key == kKeyTarget ||
                      key == kKeyCreds)) {
        log::warn(log::Area::App, "config {}:{}: '{}' is not valid in [default], ignoring", path_,
                  lineNo, key);
        return true;
    }

    const KeySetter set{path_, lineNo, error};
    if (key == kKeyId) {
        pc.id = std::string(value);
    } else if (key == kKeyMac) {
        std::array<uint8_t, 6> mac{};
        if (!set.macKey(value, mac)) return false;
        pc.mac = mac;
        section->mac = mac;
        section->hasMac = true;
    } else if (key == kKeyName) {
        pc.name = std::string(value);
        section->hasNameOverride = true;
    } else if (key == kKeyTarget) {
        const auto colon = value.find(':');
        if (colon != std::string_view::npos) {
            const auto port = parsePort(value.substr(colon + 1));
            if (!port) return set.fail("target port must be 1-65535");
            pc.targetHost = std::string(value.substr(0, colon));
            pc.targetPort = *port;
        } else {
            pc.targetHost = std::string(value);
        }
    } else if (key == kKeyProtocol) {
        if (value == kProtocolAp1)
            pc.airplay2 = false;
        else if (value == kProtocolAp2)
            pc.airplay2 = true;
        else
            return set.fail("protocol must be ap1|ap2");
    } else if (key == kKeyPassword) {
        pc.password = std::string(value);
    } else if (key == kKeyEnabled) {
        bool b = false;
        if (!set.boolKey(value, b, "enabled")) return false;
        pc.enabled = b;
    } else if (key == kKeyVolume) {
        if (value == kVolumeModeLms)
            pc.volumeMode = VolumeMode::Lms;
        else if (value == kVolumeModeFixed)
            pc.volumeMode = VolumeMode::Fixed;
        else
            return set.fail("volume must be lms|fixed");
    } else if (key == kKeyVolumeMap) {
        if (!VolumeAnchors::parse(value))
            return set.fail("volume-map needs \"db:pct, ...\" pairs, ascending pct 1-100, db <= 0");
        pc.volumeMap = std::string(value);
    } else if (key == kKeyVolumePct) {
        float parsed = 0.0f;
        if (!set.numberKey(value, parsed, kVolumePctMin, kVolumePctMax, kKeyVolumePct,
                           "0.5-100 (0 would be mute)"))
            return false;
        pc.volPct = parsed;
    } else if (key == kKeyLatencyMs) {
        int parsed = 0;
        if (!set.numberKey(value, parsed, kLatencyMinMs, kLatencyMaxMs, kKeyLatencyMs,
                           "250-2000 (receiver latencyMin..Max)"))
            return false;
        pc.latencyMs = parsed;
    } else if (key == kKeySink) {
        pc.sinkPath = std::string(value);
    } else if (key == kKeyPace) {
        if (value == kPaceFast)
            pc.paceRealtime = false;
        else if (value == kPaceRealtime)
            pc.paceRealtime = true;
        else
            return set.fail("pace must be realtime|fast");
    } else if (key == kKeyCreds) {
        section->creds = std::string(value);
    } else if (key == kKeyAuto) {
        bool b = false;
        if (!set.boolKey(value, b, "auto")) return false;
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
            if (canonical == kSectionGlobal) {
                current = Kind::Global;
                currentSection = nullptr;
            } else if (canonical == kSectionDefault) {
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
        const std::string keyRaw = toLower(trimView(std::string_view(raw).substr(0, eq)));
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
        const std::string canonical = hex ? toLower(key) : key;
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
           "\n["
        << kSectionGlobal << "]\n"
        << "# " << kKeyLms << " = 192.168.1.10:" << kDefaultLmsPort
        << "   (omit for UDP discovery on " << kDefaultLmsPort << ")\n"
        << kKeyDiscovery
        << " = on\n"
           "# iface = eth0\n"
           "# mdns-debug = off\n"
        << "# " << kKeyServerTimeoutMs << " = " << kDefaultServerTimeoutMs << "\n"
        << "# " << kKeySourceTimeoutMs << " = " << kDefaultSourceTimeoutMs
        << "\n"
           "# tls-verify = on\n"
           "# tls-ca = /etc/ssl/certs/ca-certificates.crt\n"
        << kKeyLog << " = info\n"
        << kKeyAutoRegister << " = on\n"
        << "# " << kKeyVolumeFeedback
        << " = on\n"
           "\n["
        << kSectionDefault << "]\n"
        << kKeyProtocol << " = " << kProtocolAp2
        << "\n"
           "# password =\n"
        << kKeyEnabled << " = on\n"
        << kKeyVolume << " = " << kVolumeModeLms << "\n"
        << kKeyVolumeMap << " = " << kDefaultVolumeMap << "\n"
        << kKeyVolumePct << " = " << kDefaultVolumePct << "\n"
        << kKeyLatencyMs << " = " << kDefaultLatencyMs
        << "\n"
           "# sink =\n"
        << kKeyPace << " = " << kPaceRealtime << "\n";
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

    const auto isManaged = [](std::string_view k) {
        return std::ranges::find(kManagedKeys, k) != std::end(kManagedKeys);
    };

    std::set<std::string> headerSeen;
    std::set<std::string> hasMacLine, hasCredsLine, hasAutoLine, hasNameLine;
    for (const auto& l : lines_) {
        if (l.kind == Line::Kind::Section) headerSeen.insert(l.section);
        if (l.kind == Line::Kind::Key) {
            if (l.key == kKeyMac) hasMacLine.insert(l.section);
            if (l.key == kKeyCreds) hasCredsLine.insert(l.section);
            if (l.key == kKeyAuto) hasAutoLine.insert(l.section);
            if (l.key == kKeyName) hasNameLine.insert(l.section);
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
            if (l.kind == Line::Kind::Key && isManaged(l.key)) {
                const Section* s = sectionByName(l.section);
                if (!s) {
                    out << l.raw << "\n";
                    continue;
                }
                if (l.key == kKeyMac) {
                    if (s->hasMac) out << l.valuePrefix << macToString(s->mac) << "\n";
                } else if (l.key == kKeyCreds) {
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
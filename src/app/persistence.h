#pragma once

#include "app/config.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace squeeze2raop2 {

// Owns the combined config/state file: user-authored settings ([global],
// [default], [player "..."]) plus program-managed per-player state (virtual
// MAC and AirPlay pairing credentials). Saving rewrites only the machine-owned
// keys in place, preserving every other line (comments, ordering, unknown
// keys) verbatim.
class Persistence {
public:
    // Reads path, resolving [global] + [default] + per-player inheritance into
    // `out`. Creates the file (or imports a legacy .state) when missing.
    bool open(const std::string& path, Settings& out, std::string& error);

    // Matches a discovered/static device to a player section by id, then MAC,
    // then name. When no section matches and autoRegister is set, creates a
    // minimal `auto = true` section (assigned MAC). Returns nullopt otherwise.
    std::optional<ResolvedPlayerConfig> resolve(const std::string& deviceId,
                                                const std::string& deviceName, bool autoRegister);

    // User-authored (non-auto) players, resolved and MAC-assigned, in file
    // order. These spawn at startup regardless of discovery.
    std::vector<ResolvedPlayerConfig> staticPlayers();

    std::optional<std::string> credsFor(const std::string& key) const;
    void saveCreds(const std::string& key, const std::string& credsJson);
    // Persist a player rename (LMS 'setd') as a machine-managed `name` key,
    // keeping the section header/key (and its mac/creds) stable.
    void savePlayerName(const std::string& key, const std::string& name);

    const std::string& path() const { return path_; }

private:
    struct Section {
        std::string name;     // section header name
        PlayerConfig config;  // raw fields (name/state mirrored below)
        bool autoRegistered = false;
        bool hasMac = false;
        std::array<uint8_t, 6> mac{};
        std::string creds;
        bool hasNameOverride = false;
    };

    struct Line {
        enum class Kind : std::uint8_t { Blank, Comment, Section, Key, Other };
        Kind kind = Kind::Blank;
        std::string raw;
        std::string section;  // Key: owning section; Section: canonical name
        std::string key;
        std::string value;        // parsed (unquoted) value
        std::string valuePrefix;  // raw prefix through the start of the value
    };

    bool parse(const std::string& text, Settings& out, std::string& error);
    bool importLegacy(const std::string& path, std::string& error);
    void writeTemplate();
    bool saveLocked();

    Section* findSection(const std::string& key);
    const Section* findSection(const std::string& key) const;
    Section& ensureSection(const std::string& key, const std::string& name, bool autoRegistered);
    void assignMacLocked(Section& s);
    ResolvedPlayerConfig finalizeSection(Section& s) const;

    std::string path_;
    std::vector<Line> lines_;
    std::deque<Section> sections_;
    PlayerConfig defaults_;
    GlobalConfig global_;
    bool dirty_ = false;
    mutable std::mutex mutex_;
};

}  // namespace squeeze2raop2
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace squeeze2raop2 {

struct StateStoreEntry {
    std::array<uint8_t, 6> mac{};
    bool hasMac = false;
    std::string creds;
};

class StateStore {
public:
    bool open(const std::string& path, std::string& errorOut);

    // Stable virtual MAC per device id; assigns + persists when missing.
    std::array<uint8_t, 6> macFor(const std::string& deviceId, bool& newlyAssigned);

    std::optional<std::string> credsFor(const std::string& deviceId) const;
    void saveCreds(const std::string& deviceId, const std::string& credsJson);

    const std::string& path() const { return path_; }

private:
    bool load(std::string& errorOut);
    // Callers must hold mutex_; save() itself does not lock.
    bool save();

    std::string path_;
    std::map<std::string, StateStoreEntry> entries_;
    // Guards entries_ and path_: macFor()/saveCreds() run on mDNS/sender
    // threads, load() at startup.
    mutable std::mutex mutex_;
};

} // namespace squeeze2raop2

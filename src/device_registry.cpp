#include "device_registry.h"

#include "log.h"

#include <cctype>
#include <charconv>
#include <optional>
#include <utility>

namespace squeeze2raop2 {

std::string DeviceRegistry::normalizeHexKey(const std::string& raw) {
    std::string hex;
    hex.reserve(raw.size());
    for (char c : raw) {
        unsigned char u = static_cast<unsigned char>(c);
        if (!std::isxdigit(u)) continue;
        hex.push_back(static_cast<char>(std::tolower(u)));
    }
    if (hex.size() < 12) return std::string();
    return hex.substr(0, 12);
}

std::string DeviceRegistry::keyFor(const std::string& instance) {
    if (instance.size() >= 12) {
        bool hexy = true;
        for (size_t i = 0; i < 12; ++i) {
            unsigned char c = static_cast<unsigned char>(instance[i]);
            if (!std::isxdigit(c)) {
                hexy = false;
                break;
            }
        }
        if (hexy && (instance.size() == 12 || instance[12] == '@' ||
                     instance[12] == '.' || instance[12] == '_' || instance[12] == ' '))
            return normalizeHexKey(instance.substr(0, 12));
    }
    return instance;
}

void DeviceRegistry::notify(Event ev, const AirplayDevice& d) {
    if (cb_) cb_(ev, d);
}

void DeviceRegistry::onRaopV4(const std::string& instance, const std::string& host,
                              uint16_t port, const std::map<std::string, std::string>& txt) {
    bool added = false;
    AirplayDevice snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = keyFor(instance);
        added = devices_.count(key) == 0;
        State& st = devices_[key];

        st.device.id = key;
        st.device.raopInstance = instance;
        st.device.host = host;
        st.device.raopPort = port;
        st.lastSeenRaop = true;

        {
            size_t at = instance.find('@');
            if (at != std::string::npos && at + 1 < instance.size())
                st.device.name = instance.substr(at + 1);
        }
        if (auto it = txt.find("am"); it != txt.end() && !it->second.empty())
            st.device.model = it->second;
        if (auto it = txt.find("pw"); it != txt.end())
            st.device.pw = (it->second == "true" || it->second == "1");
        if (auto it = txt.find("sf"); it != txt.end()) {
            // Preserve the legacy strtoull semantics: garbage input parses as 0.
            uint64_t sf = 0;
            std::from_chars(it->second.data(), it->second.data() + it->second.size(), sf, 16);
            st.device.encrypted = (sf & 0x2) != 0;
        }
        if (auto it = txt.find("et"); it != txt.end())
            st.device.encrypted = st.device.encrypted || it->second.find('1') != std::string::npos;

        snapshot = st.device;
    }
    // Notify without holding mutex_: callbacks build/destroy whole player
    // sessions; re-entering the registry from one must not deadlock.
    log::info("registry: raop record {}: {} name='{}' port={} encrypted={} pw={} model={}",
              added ? "added" : "updated", snapshot.id, snapshot.name, port,
              snapshot.encrypted, snapshot.pw, snapshot.model);
    notify(added ? Event::Added : Event::Updated, snapshot);
}

void DeviceRegistry::onAirplayV4(const std::string& instance, const std::string& host,
                                 uint16_t port,
                                 const std::map<std::string, std::string>& txt) {
    bool added = false;
    AirplayDevice snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key;
        // the 12-hex deviceid in the TXT record is the shared identity that
        // matches the raop instance prefix; use it so both records merge
        if (auto it = txt.find("deviceid"); it != txt.end())
            key = normalizeHexKey(it->second);
        if (key.empty()) key = keyFor(instance);
        added = devices_.count(key) == 0;
        State& st = devices_[key];

        st.device.id = key;
        st.device.apInstance = instance;
        st.device.host = host;
        st.device.airplayPort = port;
        st.lastSeenAirplay = true;

        if (st.device.name.empty()) st.device.name = instance;

        if (auto it = txt.find("features"); it != txt.end() && !it->second.empty()) {
            // Preserve the legacy strtoull semantics: garbage input parses as 0.
            uint64_t features = 0;
            std::from_chars(it->second.data(), it->second.data() + it->second.size(), features, 16);
            st.device.features = features;
        }
        if (auto it = txt.find("pk"); it != txt.end()) st.device.pk = it->second;
        if (auto it = txt.find("deviceid"); it != txt.end()) st.device.deviceIdHex = it->second;
        if (auto it = txt.find("model"); it != txt.end() && st.device.model.empty())
            st.device.model = it->second;
        if (auto it = txt.find("pw"); it != txt.end() && !st.device.pw)
            st.device.pw = (it->second == "true" || it->second == "1");

        snapshot = st.device;
    }
    // Notify without holding mutex_: callbacks build/destroy whole player
    // sessions; re-entering the registry from one must not deadlock.
    log::info("registry: airplay record {}: {} name='{}' port={} features=0x{:x} pk={} pw={}",
              added ? "added" : "updated", snapshot.id, snapshot.name, port,
              snapshot.features, snapshot.pk.empty() ? "-" : "present", snapshot.pw);
    notify(added ? Event::Added : Event::Updated, snapshot);
}

void DeviceRegistry::onRaopGone(const std::string& instance) {
    std::optional<std::pair<Event, AirplayDevice>> notifyData;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = keyFor(instance);
        auto it = devices_.find(key);
        if (it == devices_.end()) return;
        it->second.device.raopPort = 0;
        it->second.lastSeenRaop = false;
        if (!it->second.lastSeenAirplay) {
            notifyData = {Event::Removed, it->second.device};
            devices_.erase(it);
        } else {
            notifyData = {Event::Updated, it->second.device};
        }
    }
    // Notify without holding mutex_ (see onRaopV4).
    if (notifyData->first == Event::Removed)
        log::info("registry: removed {} (raop gone)", notifyData->second.id);
    notify(notifyData->first, notifyData->second);
}

void DeviceRegistry::onAirplayGone(const std::string& instance) {
    std::optional<std::pair<Event, AirplayDevice>> notifyData;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = keyFor(instance);
        auto it = devices_.find(key);
        if (it == devices_.end()) return;
        it->second.device.airplayPort = 0;
        it->second.lastSeenAirplay = false;
        if (!it->second.lastSeenRaop) {
            notifyData = {Event::Removed, it->second.device};
            devices_.erase(it);
        } else {
            notifyData = {Event::Updated, it->second.device};
        }
    }
    // Notify without holding mutex_ (see onRaopV4).
    if (notifyData->first == Event::Removed)
        log::info("registry: removed {} (airplay gone)", notifyData->second.id);
    notify(notifyData->first, notifyData->second);
}

} // namespace squeeze2raop2

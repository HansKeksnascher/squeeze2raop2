#include "device_registry.h"

#include "log.h"

#include <algorithm>
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
        const std::string_view head(instance.data(), 12);
        const bool hexy = std::ranges::all_of(
            head, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
        if (hexy && (instance.size() == 12 || instance[12] == '@' || instance[12] == '.' ||
                     instance[12] == '_' || instance[12] == ' '))
            return normalizeHexKey(instance.substr(0, 12));
    }
    return instance;
}

void DeviceRegistry::notify(Event ev, const AirplayDevice& d) {
    if (cb_) cb_(ev, d);
}

std::pair<AirplayDevice, bool> DeviceRegistry::upsertAndNotifyKey(
    const std::string& key, const std::function<void(State&)>& mutate) {
    bool added = false;
    AirplayDevice snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        added = devices_.count(key) == 0;
        State& st = devices_[key];
        mutate(st);
        st.device.id = key;
        snapshot = st.device;
    }
    return {snapshot, added};
}

std::optional<std::pair<DeviceRegistry::Event, AirplayDevice>> DeviceRegistry::markGone(
    const std::string& key, bool raop) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(key);
    if (it == devices_.end()) return std::nullopt;
    State& st = it->second;
    if (raop) {
        st.device.raopPort = 0;
        st.lastSeenRaop = false;
    } else {
        st.device.airplayPort = 0;
        st.lastSeenAirplay = false;
    }
    const bool otherRemains = raop ? st.lastSeenAirplay : st.lastSeenRaop;
    std::pair<Event, AirplayDevice> out{otherRemains ? Event::Updated : Event::Removed, st.device};
    if (out.first == Event::Removed) devices_.erase(it);
    return out;
}

void DeviceRegistry::onRaopV4(const std::string& instance, const std::string& host, uint16_t port,
                              const std::map<std::string, std::string>& txt) {
    auto [snapshot, added] = upsertAndNotifyKey(keyFor(instance), [&](State& st) {
        AirplayDevice& d = st.device;
        d.raopInstance = instance;
        d.host = host;
        d.raopPort = port;
        st.lastSeenRaop = true;

        size_t at = instance.find('@');
        if (at != std::string::npos && at + 1 < instance.size()) d.name = instance.substr(at + 1);
        if (auto it = txt.find("am"); it != txt.end() && !it->second.empty()) d.model = it->second;
        if (auto it = txt.find("pw"); it != txt.end())
            d.pw = (it->second == "true" || it->second == "1");
        if (auto it = txt.find("sf"); it != txt.end()) {
            // Preserve the legacy strtoull semantics: garbage input parses as 0.
            uint64_t sf = 0;
            std::from_chars(it->second.data(), it->second.data() + it->second.size(), sf, 16);
            d.encrypted = (sf & 0x2) != 0;
        }
        if (auto it = txt.find("et"); it != txt.end())
            d.encrypted = d.encrypted || it->second.find('1') != std::string::npos;
    });
    // Notify without holding mutex_: callbacks build/destroy whole player
    // sessions; re-entering the registry from one must not deadlock.
    log::info("registry: raop record {}: {} name='{}' port={} encrypted={} pw={} model={}",
              added ? "added" : "updated", snapshot.id, snapshot.name, port, snapshot.encrypted,
              snapshot.pw, snapshot.model);
    notify(added ? Event::Added : Event::Updated, snapshot);
}

void DeviceRegistry::onAirplayV4(const std::string& instance, const std::string& host,
                                 uint16_t port, const std::map<std::string, std::string>& txt) {
    // the 12-hex deviceid in the TXT record is the shared identity that
    // matches the raop instance prefix; use it so both records merge
    std::string key;
    if (auto it = txt.find("deviceid"); it != txt.end()) key = normalizeHexKey(it->second);
    if (key.empty()) key = keyFor(instance);

    auto [snapshot, added] = upsertAndNotifyKey(key, [&](State& st) {
        AirplayDevice& d = st.device;
        d.apInstance = instance;
        d.host = host;
        d.airplayPort = port;
        st.lastSeenAirplay = true;

        if (d.name.empty()) d.name = instance;
        if (auto it = txt.find("features"); it != txt.end() && !it->second.empty()) {
            // Preserve the legacy strtoull semantics: garbage input parses as 0.
            uint64_t features = 0;
            std::from_chars(it->second.data(), it->second.data() + it->second.size(), features, 16);
            d.features = features;
        }
        if (auto it = txt.find("pk"); it != txt.end()) d.pk = it->second;
        if (auto it = txt.find("deviceid"); it != txt.end()) d.deviceIdHex = it->second;
        if (auto it = txt.find("model"); it != txt.end() && d.model.empty()) d.model = it->second;
        if (auto it = txt.find("pw"); it != txt.end() && !d.pw)
            d.pw = (it->second == "true" || it->second == "1");
    });
    // Notify without holding mutex_ (see onRaopV4).
    log::info("registry: airplay record {}: {} name='{}' port={} features=0x{:x} pk={} pw={}",
              added ? "added" : "updated", snapshot.id, snapshot.name, port, snapshot.features,
              snapshot.pk.empty() ? "-" : "present", snapshot.pw);
    notify(added ? Event::Added : Event::Updated, snapshot);
}

void DeviceRegistry::onRaopGone(const std::string& instance) {
    auto marked = markGone(keyFor(instance), true);
    if (!marked) return;
    if (marked->first == Event::Removed)
        log::info("registry: removed {} (raop gone)", marked->second.id);
    notify(marked->first, marked->second);
}

void DeviceRegistry::onAirplayGone(const std::string& instance) {
    auto marked = markGone(keyFor(instance), false);
    if (!marked) return;
    if (marked->first == Event::Removed)
        log::info("registry: removed {} (airplay gone)", marked->second.id);
    notify(marked->first, marked->second);
}

}  // namespace squeeze2raop2

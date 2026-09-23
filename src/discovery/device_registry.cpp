#include "discovery/device_registry.h"

#include "common/log.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace squeeze2raop2 {

namespace {

// AirPlay `features` TXT is a comma-separated pair of 32-bit hex words, low
// word first: "0xAAAAAAAA,0xBBBBBBBB" -> low | (high << 32). std::from_chars
// with base 16 does NOT accept the "0x" prefix and stops at the comma, so the
// old code read the low word as 0 and every receiver looked feature-less
// (bit 38/48 never set -> everything classified AP1). Accept the prefix, the
// comma and any number of leading hex digits (strtoull semantics: trailing
// garbage is ignored, a non-hex leading char yields 0).
uint64_t parseAirplayFeatures(std::string_view raw) {
    uint64_t features = 0;
    size_t pos = 0;
    for (int word = 0; word < 2 && pos <= raw.size(); ++word) {
        const size_t comma = raw.find(',', pos);
        std::string_view part =
            raw.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
            part.remove_prefix(1);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
            part.remove_suffix(1);
        if (part.size() > 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X'))
            part.remove_prefix(2);
        uint32_t v = 0;
        if (!part.empty()) {
            uint64_t parsed = 0;
            const auto res = std::from_chars(part.data(), part.data() + part.size(), parsed, 16);
            if (res.ec == std::errc()) v = static_cast<uint32_t>(parsed & 0xFFFFFFFFu);
        }
        features |= static_cast<uint64_t>(v) << (32 * word);
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return features;
}

}  // namespace

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
        if (auto it = txt.find("features"); it != txt.end() && !it->second.empty())
            d.features = parseAirplayFeatures(it->second);
        if (auto it = txt.find("pk"); it != txt.end()) d.pk = it->second;
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

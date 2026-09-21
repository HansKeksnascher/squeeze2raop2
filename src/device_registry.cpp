#include "device_registry.h"

#include "log.h"

#include <cctype>
#include <cstdlib>

namespace sq2 {

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
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = keyFor(instance);
    bool added = devices_.count(key) == 0;
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
        uint64_t sf = std::strtoull(it->second.c_str(), nullptr, 16);
        st.device.encrypted = (sf & 0x2) != 0;
    }
    if (auto it = txt.find("et"); it != txt.end())
        st.device.encrypted = st.device.encrypted || it->second.find('1') != std::string::npos;

    log::info("registry: raop record {}: {} name='{}' port={} encrypted={} pw={} model={}",
              added ? "added" : "updated", key, st.device.name, port,
              st.device.encrypted, st.device.pw, st.device.model);
    notify(added ? Event::Added : Event::Updated, st.device);
}

void DeviceRegistry::onAirplayV4(const std::string& instance, const std::string& host,
                                 uint16_t port,
                                 const std::map<std::string, std::string>& txt) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key;
    // the 12-hex deviceid in the TXT record is the shared identity that
    // matches the raop instance prefix; use it so both records merge
    if (auto it = txt.find("deviceid"); it != txt.end())
        key = normalizeHexKey(it->second);
    if (key.empty()) key = keyFor(instance);
    bool added = devices_.count(key) == 0;
    State& st = devices_[key];

    st.device.id = key;
    st.device.apInstance = instance;
    st.device.host = host;
    st.device.airplayPort = port;
    st.lastSeenAirplay = true;

    if (st.device.name.empty()) st.device.name = instance;

    if (auto it = txt.find("features"); it != txt.end() && !it->second.empty())
        st.device.features = std::strtoull(it->second.c_str(), nullptr, 16);
    if (auto it = txt.find("pk"); it != txt.end()) st.device.pk = it->second;
    if (auto it = txt.find("deviceid"); it != txt.end()) st.device.deviceIdHex = it->second;
    if (auto it = txt.find("model"); it != txt.end() && st.device.model.empty())
        st.device.model = it->second;
    if (auto it = txt.find("pw"); it != txt.end() && !st.device.pw)
        st.device.pw = (it->second == "true" || it->second == "1");

    log::info("registry: airplay record {}: {} name='{}' port={} features=0x{:x} pk={} pw={}",
              added ? "added" : "updated", key, st.device.name, port,
              st.device.features, st.device.pk.empty() ? "-" : "present", st.device.pw);
    notify(added ? Event::Added : Event::Updated, st.device);
}

void DeviceRegistry::onRaopGone(const std::string& instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = keyFor(instance);
    auto it = devices_.find(key);
    if (it == devices_.end()) return;
    it->second.device.raopPort = 0;
    it->second.lastSeenRaop = false;
    if (!it->second.lastSeenAirplay) {
        AirplayDevice d = it->second.device;
        devices_.erase(it);
        log::info("registry: removed {} (raop gone)", key);
        notify(Event::Removed, d);
    } else {
        notify(Event::Updated, it->second.device);
    }
}

void DeviceRegistry::onAirplayGone(const std::string& instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = keyFor(instance);
    auto it = devices_.find(key);
    if (it == devices_.end()) return;
    it->second.device.airplayPort = 0;
    it->second.lastSeenAirplay = false;
    if (!it->second.lastSeenRaop) {
        AirplayDevice d = it->second.device;
        devices_.erase(it);
        log::info("registry: removed {} (airplay gone)", key);
        notify(Event::Removed, d);
    } else {
        notify(Event::Updated, it->second.device);
    }
}

}

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace sq2 {

struct AirplayDevice {
    std::string id;
    std::string name;
    std::string raopInstance;
    std::string apInstance;
    std::string host;
    uint16_t raopPort = 0;
    uint16_t airplayPort = 0;
    uint64_t features = 0;
    std::string pk;
    std::string deviceIdHex;
    std::string model;
    bool pw = false;
    bool encrypted = false;
    bool af_float = false;

    bool hasRaop() const { return raopPort != 0; }
    bool hasAirplay() const { return airplayPort != 0; }
    bool airplay2() const { return (features & (1ULL << 38)) != 0 || (features & 1ULL << 48) != 0; }
};

class DeviceRegistry {
public:
    enum class Event { Added, Updated, Removed };

    using Callback = std::function<void(Event, const AirplayDevice&)>;

    void setCallback(Callback cb) { cb_ = std::move(cb); }

    void onRaopV4(const std::string& instance, const std::string& host, uint16_t port,
                  const std::map<std::string, std::string>& txt);
    void onAirplayV4(const std::string& instance, const std::string& host, uint16_t port,
                     const std::map<std::string, std::string>& txt);
    void onRaopGone(const std::string& instance);
    void onAirplayGone(const std::string& instance);

private:
    struct State {
        AirplayDevice device;
        bool lastSeenRaop = false;
        bool lastSeenAirplay = false;
    };

    void notify(Event ev, const AirplayDevice& d);
    static std::string keyFor(const std::string& instance);
    static std::string normalizeHexKey(const std::string& raw);

    std::mutex mutex_;
    std::map<std::string, State> devices_;
    Callback cb_;
};

}

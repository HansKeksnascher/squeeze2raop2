#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace squeeze2raop2 {

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
    std::string model;
    bool pw = false;
    bool encrypted = false;

    bool hasRaop() const { return raopPort != 0; }
    bool hasAirplay() const { return airplayPort != 0; }
    bool airplay2() const {
        return (features & (1ULL << 38)) != 0 || (features & (1ULL << 48)) != 0;
    }
    // Native AirPlay 2 only when the `_airplay._tcp` record is present AND
    // advertises the HK bits; otherwise classic RAOP (squeeze2raop2 prefers
    // AirPlay 2 and falls back).
    bool useAirplay2() const { return hasAirplay() && airplay2(); }
    // Port to reach the receiver over the chosen transport. Falls back to the
    // other service if the preferred one is missing.
    uint16_t preferredPort() const {
        return useAirplay2() ? airplayPort : (raopPort ? raopPort : airplayPort);
    }
};

class DeviceRegistry {
public:
    enum class Event : std::uint8_t { Added, Updated, Removed };

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
    // Common upsert scaffold: creates/updates State for `key` via `mutate`
    // under the registry mutex, snapshots the device and reports whether it
    // was newly added. Notification is the caller's job so its log line
    // stays ordered before the callback.
    std::pair<AirplayDevice, bool> upsertAndNotifyKey(const std::string& key,
                                                      const std::function<void(State&)>& mutate);
    // Marks one service type gone and erases the device when neither
    // remains; nullopt when the key is unknown.
    std::optional<std::pair<Event, AirplayDevice>> markGone(const std::string& key, bool raop);
    static std::string keyFor(const std::string& instance);
    static std::string normalizeHexKey(const std::string& raw);

    std::mutex mutex_;
    std::map<std::string, State> devices_;
    Callback cb_;
};

}  // namespace squeeze2raop2

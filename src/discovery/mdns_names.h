#pragma once

#include <cstddef>
#include <string_view>

namespace squeeze2raop2 {

// mDNS service types browsed for AirPlay receivers.
constexpr std::string_view kRaopService = "_raop._tcp";
constexpr std::string_view kAirplayService = "_airplay._tcp";
// The mDNS local domain suffix.
constexpr std::string_view kLocalSuffix = ".local";

// Trims a trailing ".local" and the ".{serviceType}" suffix from an mDNS PTR
// target, yielding the service instance name. Returns the (trailing-dot
// trimmed) input unchanged when it does not carry that suffix. Pure, so it can
// be unit tested without the mDNS core.
inline std::string_view stripServiceSuffix(std::string_view fqdnIn, std::string_view serviceType) {
    std::string_view fqdn = fqdnIn;
    while (fqdn.size() > 1 && fqdn.back() == '.') fqdn.remove_suffix(1);
    // Compare against "." + serviceType + ".local" without allocating it.
    const std::size_t suffixLen = 1 + serviceType.size() + kLocalSuffix.size();
    if (fqdn.size() < suffixLen) return fqdn;
    const std::string_view tail = fqdn.substr(fqdn.size() - suffixLen);
    if (tail[0] != '.') return fqdn;
    if (tail.substr(1, serviceType.size()) != serviceType) return fqdn;
    if (tail.substr(1 + serviceType.size()) != kLocalSuffix) return fqdn;
    return fqdn.substr(0, fqdn.size() - suffixLen);
}

}  // namespace squeeze2raop2
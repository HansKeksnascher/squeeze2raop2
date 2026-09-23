#pragma once

#include <cstddef>
#include <string_view>

namespace squeeze2raop2 {

// Trims a trailing ".local" and the ".{serviceType}" suffix from an mDNS PTR
// target, yielding the service instance name. Returns the (trailing-dot
// trimmed) input unchanged when it does not carry that suffix. Pure, so it can
// be unit tested without the mDNS core.
inline std::string_view stripServiceSuffix(std::string_view fqdnIn, std::string_view serviceType) {
    std::string_view fqdn = fqdnIn;
    while (fqdn.size() > 1 && fqdn.back() == '.') fqdn.remove_suffix(1);
    // Compare against "." + serviceType + ".local" without allocating it.
    constexpr std::string_view kLocal = ".local";
    const std::size_t suffixLen = 1 + serviceType.size() + kLocal.size();
    if (fqdn.size() < suffixLen) return fqdn;
    const std::string_view tail = fqdn.substr(fqdn.size() - suffixLen);
    if (tail[0] != '.') return fqdn;
    if (tail.substr(1, serviceType.size()) != serviceType) return fqdn;
    if (tail.substr(1 + serviceType.size()) != kLocal) return fqdn;
    return fqdn.substr(0, fqdn.size() - suffixLen);
}

}  // namespace squeeze2raop2
#include "discovery/mdns_names.h"

#include "check.h"

#include <string_view>

using namespace squeeze2raop2;
using namespace squeeze2raop2::test;

SQ2_TEST(mdns_names, strip_service_suffix) {
    using std::string_view;
    expect(stripServiceSuffix("AABBCC@Kueche._raop._tcp.local.", "_raop._tcp") == "AABBCC@Kueche",
           "strips ._raop._tcp.local and the trailing dot");
    expect(stripServiceSuffix("Kueche._airplay._tcp.local", "_airplay._tcp") == "Kueche",
           "strips without a trailing dot");
    expect(stripServiceSuffix("Kueche._raop._tcp.local.", "_raop._tcp") == "Kueche",
           "tolerates multiple trailing dots");
    expect(
        stripServiceSuffix("Kueche._raop._tcp.local", "_airplay._tcp") == "Kueche._raop._tcp.local",
        "mismatched service type returns the trimmed fqdn");
    expect(stripServiceSuffix("printer.local", "_raop._tcp") == "printer.local",
           "name without the service prefix is returned trimmed");
    expect(stripServiceSuffix("_raop._tcp.local", "_raop._tcp") == "_raop._tcp.local",
           "missing instance is not stripped");
    expect(stripServiceSuffix(".", "_raop._tcp") == ".", "a lone dot is preserved");
    expect(stripServiceSuffix("", "_raop._tcp").empty(), "empty input stays empty");
}
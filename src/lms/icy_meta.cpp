#include "lms/icy_meta.h"

namespace squeeze2raop2 {

std::string withIcyRequestHeader(std::string request) {
    if (request.find("Icy-MetaData") != std::string::npos) return request;

    constexpr std::string_view hdr = "Icy-MetaData: 1\r\n";
    const auto end = request.find("\r\n\r\n");
    if (end != std::string::npos) {
        // Insert the header inside the header block, before the blank line.
        request.insert(end + 2, hdr);
    } else if (request.size() >= 2 && request.compare(request.size() - 2, 2, "\r\n") == 0) {
        request += hdr;
    } else {
        request += "\r\n";
        request += hdr;
    }
    return request;
}

std::optional<std::string> parseStreamTitle(std::string_view block) {
    constexpr std::string_view key = "StreamTitle='";
    const auto p = block.find(key);
    if (p == std::string_view::npos) return std::nullopt;
    const auto e = block.find('\'', p + key.size());
    if (e == std::string_view::npos) return std::nullopt;
    std::string title(block.substr(p + key.size(), e - p - key.size()));
    if (title.empty()) return std::nullopt;
    return title;
}

}  // namespace squeeze2raop2

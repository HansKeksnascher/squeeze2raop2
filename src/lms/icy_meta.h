#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace squeeze2raop2 {

// Ensure an HTTP stream request asks for in-band ICY metadata. LMS's
// /stream.mp3 only interleaves StreamTitle blocks when the client sends
// Icy-MetaData: 1 — the same request LMS itself makes to remote servers
// (Protocols/HTTP.pm). A request that already carries the header is returned
// unchanged.
std::string withIcyRequestHeader(std::string request);

// Extract StreamTitle='...' from an ICY metadata block; nullopt when the key
// is absent, unterminated or the title is empty.
std::optional<std::string> parseStreamTitle(std::string_view block);

}  // namespace squeeze2raop2

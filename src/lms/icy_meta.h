#pragma once

#include <cstdint>
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

// The icy-metaint interval parsed from an HTTP response's header block, or 0
// when the header is absent or the value is unusable (zero, absurdly large, or
// malformed). 0 means "no in-band metadata".
uint32_t parseIcyMetaint(std::string_view headers);

// Extract StreamTitle='...' from an ICY metadata block; nullopt when the key
// is absent, unterminated or the title is empty.
std::optional<std::string> parseStreamTitle(std::string_view block);

}  // namespace squeeze2raop2

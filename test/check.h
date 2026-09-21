#pragma once

// Minimal assertion helper for the hand-rolled test executables: one line,
// no framework, exits the process on the first failure.

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace sq2t {

inline void expect(bool ok, std::string_view what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
        std::exit(1);
    }
}

} // namespace sq2t

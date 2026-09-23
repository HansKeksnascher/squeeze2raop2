#pragma once

// GCC/Clang warning suppression around vendored third-party headers (minimp3,
// libxaac). Those headers are not ours to fix, so the same suppression set is
// pushed before the include and popped after, keeping our own code under the
// project's full warning set.
//
// Kept out of clang-format: it would cascade the escaped-newline alignment.
// clang-format off
#if defined(__GNUC__)
#define SQUEEZE2RAOP2_TP_WARNINGS_PUSH                      \
    _Pragma("GCC diagnostic push")                          \
    _Pragma("GCC diagnostic ignored \"-Wold-style-cast\"")  \
    _Pragma("GCC diagnostic ignored \"-Wsign-conversion\"") \
    _Pragma("GCC diagnostic ignored \"-Wconversion\"")      \
    _Pragma("GCC diagnostic ignored \"-Wuseless-cast\"")    \
    _Pragma("GCC diagnostic ignored \"-Wcast-align\"")      \
    _Pragma("GCC diagnostic ignored \"-Wdouble-promotion\"")
#define SQUEEZE2RAOP2_TP_WARNINGS_POP _Pragma("GCC diagnostic pop")
#else
#define SQUEEZE2RAOP2_TP_WARNINGS_PUSH
#define SQUEEZE2RAOP2_TP_WARNINGS_POP
#endif
// clang-format on

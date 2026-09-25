#pragma once

#include <cstdint>

namespace squeeze2raop2 {

// Why the stream loop ended, and what the exit path should do about it.
enum class ExitAction : std::uint8_t {
    SilentStop,  // stop request / shutdown: no STAT (handlers already told LMS)
    Retry,       // receiver-initiated loss, first time: recreate and resume
    GaveUp,      // receiver loss again after the one retry: report STMd
    EndedEof,    // natural end: STMd, drain the tail, STMu
    EndedError,  // socket/decode error: report STMu
};

struct ExitInputs {
    bool stopping;    // stop_requested() || !g_run
    bool lost;        // receiver closed the session
    bool retryUsed;   // the one transparent retry is already spent
    bool reachedEof;  // HTTP EOF with the decoder drained
};

[[nodiscard]] constexpr ExitAction decideExit(const ExitInputs& in) {
    if (in.stopping) return ExitAction::SilentStop;
    if (in.lost) return in.retryUsed ? ExitAction::GaveUp : ExitAction::Retry;
    if (in.reachedEof) return ExitAction::EndedEof;
    return ExitAction::EndedError;
}

}  // namespace squeeze2raop2
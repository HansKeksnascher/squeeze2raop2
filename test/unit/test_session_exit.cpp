// Pins the pure stream-exit policy: which STAT/retry the stream loop emits
// for each combination of stop request, receiver loss and natural end.

#include "playback/player_session.h"

#include "check.h"

using namespace squeeze2raop2::test;
using squeeze2raop2::decideExit;
using squeeze2raop2::ExitAction;
using squeeze2raop2::ExitInputs;

// Pure and compile-time evaluable.
static_assert(decideExit({true, true, false, true}) == ExitAction::SilentStop);
static_assert(decideExit({false, true, false, false}) == ExitAction::Retry);
static_assert(decideExit({false, true, true, false}) == ExitAction::GaveUp);
static_assert(decideExit({false, false, false, true}) == ExitAction::EndedEof);
static_assert(decideExit({false, false, false, false}) == ExitAction::EndedError);

SQ2_TEST(session_exit, policy) {
    // A stop request always wins, whatever else is set.
    expect(decideExit({true, false, false, false}) == ExitAction::SilentStop, "stop -> silent");
    expect(decideExit({true, true, true, true}) == ExitAction::SilentStop, "stop beats all");

    // Receiver loss: one retry, then give up. Loss beats EOF.
    expect(decideExit({false, true, false, false}) == ExitAction::Retry, "first loss retries");
    expect(decideExit({false, true, false, true}) == ExitAction::Retry, "loss before eof");
    expect(decideExit({false, true, true, false}) == ExitAction::GaveUp, "second loss gives up");
    expect(decideExit({false, true, true, true}) == ExitAction::GaveUp, "give up before eof");

    // Natural end vs error.
    expect(decideExit({false, false, false, true}) == ExitAction::EndedEof, "eof -> STMd");
    expect(decideExit({false, false, false, false}) == ExitAction::EndedError,
           "error -> EndedError");
}
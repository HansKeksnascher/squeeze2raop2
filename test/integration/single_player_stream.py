#!/usr/bin/env python3
"""Single player end-to-end: fake LMS streams a track through the bridge to a
WAV file sink, then the LMS asks it to stop and the bridge reports STMu.

Replaces the old run_m1.sh (milestone-named, always exited 0)."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "sink opened" in t, what="the bridge to open the WAV sink")
    # stop_after makes the LMS send strm-q; the bridge stops silently (no STAT)
    # but reports the stream as ended.
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the bridge to report the ended stream")
    wavs = c.wav_files()
    assert wavs, "a non-empty WAV was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "single_player_stream",
        lms_args=["--stream-seconds", "4", "--volume-pct", "68", "--stop-after-sec", "1"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
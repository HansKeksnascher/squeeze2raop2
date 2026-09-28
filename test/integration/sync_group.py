#!/usr/bin/env python3
"""Sync-group participation: a `strm s` with autostart 0 holds the output at a
start gate (the bridge decodes and reports STMl but does not launch), a
scheduled `strm u` (a start time in the player's own jiffies clock) opens it,
and the periodic `strm a` / `strm p` corrections are applied without killing
the stream."""

import sys

import harness


def body(c):
    # The autostart-0 stream must hold the gate before the scheduled start.
    c.wait_bridge(lambda t: "start gate: waiting for scheduled start" in t,
                  what="the start gate to hold output")
    # The fake LMS schedules strm-u off the STMl jiffies, so the gate opens.
    c.wait_bridge(lambda t: "start gate: open, starting output" in t,
                  what="the scheduled start to open the gate")
    c.wait_lms(lambda t: "sync: STMl jiffies=" in t, what="the scheduled strm-u")
    c.wait_lms(lambda t: "sync: first STMs" in t, what="the first STMs")
    # Corrections: a skip-ahead and a timed pause (non-destructive).
    c.wait_bridge(lambda t: "skip ahead" in t, what="a strm-a correction")
    c.wait_bridge(lambda t: "pause requested: interval=150 ms" in t,
                  what="a timed strm-p correction")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the stream to end after the stop")

    bridge = c.bridge_log.read_text(errors="replace")
    # The gate must open after it was held, never before.
    assert bridge.index("start gate: waiting for scheduled start") < \
        bridge.index("start gate: open, starting output"), \
        "output must not start before the scheduled strm-u"
    assert "output underrun while stream active" not in bridge, \
        "a sync correction must not report STMo (LMS would rebuffer)"

    wavs = c.wav_files()
    assert wavs, "a non-empty WAV was written under %s" % c.workdir
    assert harness.wav_peak(wavs[0]) > 0, "audio was produced"


if __name__ == "__main__":
    case = harness.Case(
        "sync_group",
        lms_args=["--stream-seconds", "6", "--no-volume", "--sync",
                  "--sync-start-ms", "400", "--sync-skip-ms", "200",
                  "--sync-pause-ms", "150", "--stop-after-sec", "5"],
        players=["Kitchen"],
        log_level="debug",
        pace="realtime",
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))

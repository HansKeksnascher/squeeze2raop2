#!/usr/bin/env python3
"""Replay gain (strm-s replay_gain, 16.16) attenuates the PCM that reaches the
sink. The fake LMS tone has amplitude 6000; 0.5 gain should halve it."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the stream to end after the stop")
    wavs = c.wav_files()
    assert wavs, "a non-empty WAV was written under %s" % c.workdir
    peak = harness.wav_peak(wavs[0])
    assert 500 < peak < 4500, "replay gain halved the peak (got %d)" % peak


if __name__ == "__main__":
    case = harness.Case(
        "replay_gain",
        lms_args=["--stream-seconds", "2", "--replay-gain", "32768", "--no-volume",
                  "--stop-after-sec", "2"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
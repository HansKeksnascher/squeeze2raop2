#!/usr/bin/env python3
"""LMS 'strm a' (skip ahead) is accepted and applied without killing the
stream or resetting the elapsed accounting."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "skip ahead" in t, what="the bridge to apply strm-a")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the stream to end after the stop")
    assert c.wav_files(), "a non-empty WAV was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "skip_ahead",
        lms_args=["--stream-seconds", "2", "--skip-ms", "500", "--no-volume",
                  "--stop-after-sec", "2"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
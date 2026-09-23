#!/usr/bin/env python3
"""autostart>=2 with an unknown codec: the bridge opens the source, waits for
'codc', attaches the decoder, then streams normally."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "waiting for codc" in t, what="the bridge to await codc")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the codc stream to end after the stop")
    assert c.wav_files(), "a non-empty WAV was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "codc_stream",
        lms_args=["--stream-seconds", "2", "--autostart", "2", "--format", "?",
                  "--codc", "p", "--no-volume", "--stop-after-sec", "2"],
        players=["Kitchen"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
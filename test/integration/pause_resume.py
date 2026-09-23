#!/usr/bin/env python3
"""Pause and resume round-trip: the bridge acks STMp/STMr and the pump keeps
delivering audio after the resume instead of dying on the pause."""

import sys

import harness


def body(c):
    c.wait_bridge(lambda t: "pause requested" in t, what="the bridge to receive the pause")
    c.wait_lms(lambda t: "STAT STMp" in t, what="the pause ack")
    c.wait_lms(lambda t: "STAT STMr" in t, what="the resume ack")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the stream to end after the stop")
    text = c.bridge_log.read_text(errors="replace")
    assert "output underrun while stream active" not in text, \
        "a pause/resume must not report STMo (LMS would rebuffer)"
    wavs = c.wav_files()
    assert wavs, "a non-empty WAV was written under %s" % c.workdir
    assert harness.wav_peak(wavs[0]) > 3000, "audio is still present after resume"


if __name__ == "__main__":
    case = harness.Case(
        "pause_resume",
        lms_args=["--stream-seconds", "5", "--pause-after-sec", "2", "--pause-for-sec", "2",
                  "--no-volume", "--stop-after-sec", "6"],
        players=["Kitchen"],
        log_level="debug",
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
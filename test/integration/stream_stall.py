#!/usr/bin/env python3
"""A stalled HTTP source is detected by [global] source-timeout-ms: the server
stops sending but keeps the socket open, so the reader never returns EOF or a
socket error. The bridge must end the track (DSCO + STMn) so LMS re-issues the
stream, instead of reading timeouts forever with the sender ring drained."""

import sys

import harness


def body(c):
    # The fake LMS sends ~1 s of audio, then holds the connection open with no
    # further data (the half-open case that never yields Eof/Closed).
    c.wait_bridge(lambda t: "source silent for" in t, timeout=20.0,
                  what="the bridge to trip the source-stall watchdog")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=10.0,
                  what="the stalled stream to end")
    # LMS must have seen the disconnect as an error (DSCO + STMn), not a normal
    # end, so it can retry/advance.
    c.wait_lms(lambda t: "DSCO" in t, timeout=5.0,
               what="the bridge to report DSCO to LMS")
    c.wait_lms(lambda t: "STAT STMn" in t, timeout=5.0,
               what="the bridge to report STMn to LMS")
    assert c.wav_files(), "the pre-stall audio was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "stream_stall",
        lms_args=["--stream-seconds", "30", "--stall-after-sec", "1", "--no-volume"],
        players=["Kitchen"],
        global_lines=["source-timeout-ms = 2000"],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
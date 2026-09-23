#!/usr/bin/env python3
"""Queue/end-of-track: the fake LMS serves N short tracks to EOF and advances
the playlist when the bridge reports STMd ("decoder ready").

Replaces the old run_queue.sh (always exited 0)."""

import sys

import harness

TRACKS = 3


def body(c):
    c.wait_lms(lambda t: harness.count(t, "strm-s sent") >= TRACKS, timeout=40.0,
               what="all %d tracks to start" % TRACKS)
    c.wait_lms(lambda t: harness.count(t, "STMd received") >= TRACKS - 1, timeout=20.0,
               what="the queue to advance on STMd")
    text = c.lms_log.read_text(errors="replace")
    started = harness.count(text, "strm-s sent")
    advanced = harness.count(text, "STMd received")
    assert started >= TRACKS, "tracks started=%d, want %d" % (started, TRACKS)
    assert advanced >= TRACKS - 1, "STMd advances=%d, want %d" % (advanced, TRACKS - 1)


if __name__ == "__main__":
    case = harness.Case(
        "queue_advance",
        lms_args=["--stream-seconds", "1", "--queue-tracks", str(TRACKS), "--no-volume"],
        players=["QueueTest"],
    )
    case.default_sink = str(case.workdir / "queue.wav")
    sys.exit(harness.run(case, body))
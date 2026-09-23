#!/usr/bin/env python3
"""Two static players both register with the fake LMS.

Replaces the old run_m2.sh (milestone-named, always exited 0)."""

import sys

import harness


def body(c):
    c.wait_lms(lambda t: harness.count(t, "HELO device_id=12") >= 2, timeout=20.0,
               what="both players to send HELO")
    helos = harness.count(c.lms_log.read_text(errors="replace"), "HELO device_id=12")
    assert helos >= 2, "expected 2 registrations, saw %d" % helos


if __name__ == "__main__":
    case = harness.Case(
        "two_player_registration",
        players=["Kitchen", "Living Room"],
    )
    case.default_sink = str(case.workdir / "two_players.wav")
    sys.exit(harness.run(case, body))
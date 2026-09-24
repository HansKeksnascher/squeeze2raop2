#!/usr/bin/env python3
"""Direct HTTPS radio (CanHTTPS): the bridge advertises CanHTTPS=1, LMS hands
over the stream with the 0x20 TLS flag and a direct URL, and the bridge fetches
it over TLS, verifying the fake LMS certificate against the fixture CA."""

import sys

import harness

TLS_FIXTURES = harness.TEST_ROOT / "fixtures" / "tls"
CA = TLS_FIXTURES / "ca.pem"
CERT = TLS_FIXTURES / "server.pem"
KEY = TLS_FIXTURES / "server.key"


def body(c):
    c.wait_bridge(lambda t: "CanHTTPS=1" in t,
                  what="the bridge to advertise CanHTTPS=1")
    c.wait_bridge(lambda t: "TLS connect" in t,
                  what="the bridge to open the direct https stream")
    c.wait_bridge(lambda t: "decoder: pcm stream" in t,
                  what="the bridge to attach the pcm decoder")
    c.wait_bridge(lambda t: "stream ended" in t, timeout=30.0,
                  what="the https stream to end after the stop")
    assert c.wav_files(), "a non-empty WAV was written under %s" % c.workdir


if __name__ == "__main__":
    case = harness.Case(
        "https_stream",
        lms_args=["--stream-seconds", "2", "--format", "p", "--no-volume",
                  "--https-cert", str(CERT), "--https-key", str(KEY),
                  "--stop-after-sec", "2"],
        players=["Kitchen"],
        global_lines=["tls-ca = %s" % CA],
    )
    case.default_sink = str(case.workdir / "kitchen.wav")
    sys.exit(harness.run(case, body))
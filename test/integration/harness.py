"""Shared plumbing for the integration scenarios.

Each scenario spawns the real bridge against the fake LMS, waits for a
behavioural marker, asserts, and exits non-zero on failure. Unlike the old
shell harnesses (which always exited 0), these are gating CTest cases.

On failure the working directory (logs, WAVs, config) is kept and its path is
printed, so a red CTest run is debuggable.
"""

import re
import socket
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
TEST_ROOT = TEST_DIR.parent
REPO_ROOT = TEST_ROOT.parent
FAKE_LMS = TEST_ROOT / "fake_lms.py"


def free_port():
    """Pick an ephemeral loopback port (avoids clashes with a real LMS)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def bridge_binary(argv):
    if len(argv) > 1:
        return argv[1]
    return str(REPO_ROOT / "build" / "squeeze2raop2")


def count(text, marker):
    return len(re.findall(re.escape(marker), text))


class Case:
    """One integration scenario: fake LMS + bridge + behavioural waits."""

    def __init__(self, name, lms_args=None, players=None, default_sink=None,
                 log_level="info"):
        self.name = name
        self.lms_args = list(lms_args or [])
        self.players = list(players or [])
        self.default_sink = default_sink
        self.log_level = log_level

        self.workdir = Path(tempfile.mkdtemp(prefix="sq2_" + name + "_"))
        self.lms_log = self.workdir / (name + "_lms.log")
        self.bridge_log = self.workdir / (name + "_bridge.log")
        self.config_path = self.workdir / (name + ".conf")
        self.tcp_port = free_port()
        self.http_port = free_port()
        self.lms = None
        self.bridge = None
        self._lms_out = None
        self._bridge_out = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop()
        if exc_type is None:
            shutil.rmtree(self.workdir, ignore_errors=True)
        else:
            print("logs kept in %s" % self.workdir, file=sys.stderr)
        return False

    def _write_config(self):
        lines = [
            "[global]",
            "lms = 127.0.0.1:%d" % self.tcp_port,
            "discovery = off",
            "log = %s" % self.log_level,
            "",
        ]
        if self.default_sink is not None:
            lines += ["[default]", "sink = %s" % self.default_sink, "pace = fast", ""]
        for player in self.players:
            lines += ['[player "%s"]' % player, ""]
        self.config_path.write_text("\n".join(lines))

    def start(self, binary):
        self._write_config()
        self._lms_out = open(self.lms_log, "w")
        cmd = [sys.executable, str(FAKE_LMS),
               "--tcp-port", str(self.tcp_port),
               "--http-port", str(self.http_port)] + self.lms_args
        self.lms = subprocess.Popen(cmd, stdout=self._lms_out,
                                    stderr=subprocess.STDOUT, cwd=self.workdir)
        self.wait_lms(lambda t: "fake-lms tcp=" in t, timeout=10.0,
                      what="the fake LMS to listen")

        self._bridge_out = open(self.bridge_log, "w")
        self.bridge = subprocess.Popen([binary, "--config", str(self.config_path)],
                                       stdout=self._bridge_out,
                                       stderr=subprocess.STDOUT, cwd=self.workdir)

    def _read(self, path):
        try:
            return path.read_text(errors="replace")
        except FileNotFoundError:
            return ""

    def wait_lms(self, predicate, timeout=20.0, what="an LMS marker"):
        return self._wait(self.lms_log, predicate, timeout, what)

    def wait_bridge(self, predicate, timeout=20.0, what="a bridge marker"):
        return self._wait(self.bridge_log, predicate, timeout, what)

    def _wait(self, path, predicate, timeout, what):
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = self._read(path)
            if predicate(text):
                return text
            if self.bridge is not None and self.bridge.poll() is not None:
                raise AssertionError(
                    "bridge exited early (%s); log:\n%s"
                    % (self.bridge.returncode, self._read(self.bridge_log)))
            time.sleep(0.1)
        raise AssertionError("timed out waiting for %s; log:\n%s"
                             % (what, self._read(path)))

    def wav_files(self):
        return sorted(p for p in self.workdir.glob("*.wav") if p.stat().st_size > 44)

    def stop(self):
        for proc in (self.bridge, self.lms):
            if proc is None or proc.poll() is not None:
                continue
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        for f in (self._bridge_out, self._lms_out):
            if f is not None:
                f.close()


def run(case, body):
    """Drive one scenario; returns a process exit code."""
    try:
        with case:
            case.start(bridge_binary(sys.argv))
            body(case)
    except AssertionError as exc:
        print("FAIL: %s" % exc, file=sys.stderr)
        return 1
    print("PASS")
    return 0
import argparse
import math
import socket
import struct
import threading
import time


def report(msg):
    print("[lms] " + str(msg), flush=True)


def build_stream_bytes(seconds, sample_rate):
    total_bytes = int(sample_rate * seconds * 4)
    header = bytearray()
    header += b"RIFF"
    header += struct.pack("<I", total_bytes - 8 + 36)
    header += b"WAVEfmt "
    header += struct.pack("<IHHIIHH", 16, 1, 2, sample_rate, sample_rate * 4, 4, 16)
    header += b"data"
    header += struct.pack("<I", total_bytes)
    samples = bytearray()
    phase = 0.0
    while len(samples) < total_bytes:
        value = int(6000.0 * math.sin(2.0 * math.pi * phase))
        samples += struct.pack("<2h", value, value)
        phase += 440.0 / sample_rate
    return bytes(header + samples)


def slim_frame(opcode, payload=b""):
    return struct.pack(">H", len(payload) + 4) + opcode + payload


def read_frame(sock):
    data = b""
    while len(data) < 8:
        chunk = sock.recv(8 - len(data))
        if not chunk:
            return None
        data += chunk
    opcode = data[:4]
    (length,) = struct.unpack(">I", data[4:8])
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            return None
        payload += chunk
    return opcode + payload


class FakeLms:
    def __init__(self, tcp_port, http_port, stream_seconds, volume_pct, stop_after, queue_tracks=0,
                 autostart=1, fmt="p", replay_gain=0, transition=0, transition_secs=0,
                 skip_ms=0, send_aude_off=False, codc_codec=None, stall_after=0.0,
                 silent=False, pause_after=0.0, pause_for=0.0, aac_fixture=None,
                 container="adts", aac_reps=0, ogg_fixture=None, opus_fixture=None):
        self.tcp_port = tcp_port
        self.http_port = http_port
        self.stream_seconds = stream_seconds
        self.volume_pct = volume_pct
        self.stop_after = stop_after
        # queue_tracks > 0: serve N short tracks to EOF (closing each HTTP
        # connection) and send the next strm-s when the player reports STMd
        # ("decoder ready"), emulating an LMS playlist.
        self.queue_tracks = queue_tracks
        self.tracks_sent = 0
        self.mac = ""
        self.autostart = autostart
        self.fmt = fmt
        self.replay_gain = replay_gain
        self.transition = transition
        self.transition_secs = transition_secs
        self.skip_ms = skip_ms
        self.send_aude_off = send_aude_off
        self.codc_codec = codc_codec
        self.aac_fixture = aac_fixture
        self.container = container
        self.aac_reps = aac_reps
        self.ogg_fixture = ogg_fixture
        self.opus_fixture = opus_fixture
        self.stall_after = stall_after
        self.silent = silent
        self.pause_after = pause_after
        self.pause_for = pause_for
        self.pause_sent = False
        self.resume_sent = False
        self.pause_time = 0.0
        self.skip_sent = False
        self.aude_sent = False

    def handle_http(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.http_port))
        srv.listen(4)
        while True:
            conn, _ = srv.accept()
            try:
                request = b""
                while b"\r\n\r\n" not in request:
                    part = conn.recv(4096)
                    if not part:
                        break
                    request += part
                report("http %s" % request.split(b"\r\n")[0].decode(errors="replace"))
                if self.aac_fixture:
                    audio = self._aac_stream()
                    content_type = b"audio/aac" if self.container == "adts" else b"audio/mp4"
                elif self.ogg_fixture:
                    audio = open(self.ogg_fixture, "rb").read()
                    content_type = b"audio/ogg"
                elif self.opus_fixture:
                    audio = open(self.opus_fixture, "rb").read()
                    content_type = b"audio/ogg"
                else:
                    seconds = self.stream_seconds if self.queue_tracks else self.stream_seconds + 60.0
                    audio = build_stream_bytes(seconds, 44100)
                    content_type = b"audio/wav"
                conn.sendall(
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: " + content_type + b"\r\n"
                    b"Server: fake-lms\r\n"
                    b"\r\n"
                )
                sent = 0
                t0 = time.time()
                stalled = False
                while sent < len(audio):
                    if self.stall_after and time.time() - t0 > self.stall_after:
                        stalled = True
                        break
                    chunk = audio[sent:sent + 44100 * 4]
                    conn.sendall(chunk)
                    sent += len(chunk)
                    target = sent / (44100.0 * 4)
                    elapsed = time.time() - t0
                    if target > elapsed:
                        time.sleep(min(target - elapsed, 0.5))
                if stalled:
                    # Hold the connection open without data so the bridge's ring
                    # drains and it reports an output underrun (STMo).
                    while True:
                        time.sleep(0.5)
                if self.queue_tracks:
                    # Natural end of the track: EOF so the player completes
                    # decoding and reports STMd.
                    conn.shutdown(socket.SHUT_WR)
                    conn.close()
                    report("track streamed to EOF and closed")
                elif self.aac_fixture:
                    # A file-backed AAC fixture is a finite stream: close so the
                    # bridge decodes to EOF and reports the end.
                    conn.shutdown(socket.SHUT_WR)
                    conn.close()
                    report("aac fixture streamed and closed")
                elif self.ogg_fixture:
                    # A file-backed Ogg fixture is a finite stream: close so the
                    # bridge decodes to EOF and reports the end.
                    conn.shutdown(socket.SHUT_WR)
                    conn.close()
                    report("ogg fixture streamed and closed")
                elif self.opus_fixture:
                    # A file-backed Opus fixture is a finite stream: close so the
                    # bridge decodes to EOF and reports the end.
                    conn.shutdown(socket.SHUT_WR)
                    conn.close()
                    report("opus fixture streamed and closed")
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass

    def _aac_stream(self):
        # Repeat the short fixture to cover the requested duration; the exact
        # loop point does not matter for the bridge's decode assertion.
        data = open(self.aac_fixture, "rb").read()
        reps = self.aac_reps if self.aac_reps > 0 else max(1, int((self.stream_seconds + 60.0) / 0.12))
        return data * reps

    def send_strm_start(self, sock):
        unknown = self.fmt == "?"
        packed = bytearray()
        packed += b"s"      # command: start
        packed += str(self.autostart).encode()  # autostart 0-3
        packed += self.fmt.encode()             # format: 'p', 'm', 'a' or '?'
        if unknown:
            sample_size = b"?"
        elif self.fmt == "a":
            # LMS pcm_sample_size carries the AAC transport: '2' ADTS, '5' MP4.
            sample_size = b"2" if self.container == "adts" else b"5"
        else:
            sample_size = b"1"                  # 16 bit
        packed += sample_size
        packed += b"?" if unknown else b"3"     # sample rate: 44.1 kHz
        packed += b"?" if unknown else b"2"     # channels: stereo
        packed += b"?"      # endianness unknown (wav container)
        packed += b"\x02"   # threshold 2 KB
        packed += b"\x00"   # spdif
        packed += bytes([self.transition_secs & 0xFF])  # transition period
        packed += bytes([ord("0") + self.transition])   # transition type
        packed += b"\x40"   # flags: stream without restart
        packed += b"\x10"   # output threshold 1.0s
        packed += b"\x00"   # reserved/slaves
        packed += struct.pack(">I", self.replay_gain & 0xFFFFFFFF)  # replay gain
        packed += struct.pack(">H", self.http_port)
        packed += bytes([127, 0, 0, 1])         # server ip
        packed += ("GET /stream.mp3?player=%s HTTP/1.0\r\n" % self.mac.replace(":", "%3A")).encode()
        packed += b"\r\n"
        sock.sendall(slim_frame(b"strm", bytes(packed)))
        report("strm-s sent (autostart=%d, format=%s, replay-gain=%d, transition=%d/%ds)"
               % (self.autostart, self.fmt, self.replay_gain, self.transition, self.transition_secs))

    def send_codc(self, sock, codec):
        packed = bytearray()
        packed += codec.encode()  # format
        packed += b"1"            # sample size 16
        packed += b"3"            # sample rate 44.1k
        packed += b"2"            # channels stereo
        packed += b"?"            # endianness unknown (container sniff)
        sock.sendall(slim_frame(b"codc", bytes(packed)))
        report("codc sent (format=%s)" % codec)

    def send_strm_a(self, sock, ms):
        packed = bytearray()
        packed += b"a"
        packed += b"\x00" * 13
        packed += struct.pack(">I", ms)
        sock.sendall(slim_frame(b"strm", bytes(packed)))
        report("strm-a skip %d ms" % ms)

    def send_strm_p(self, sock, ms=0):
        packed = bytearray()
        packed += b"p"
        packed += b"\x00" * 13
        packed += struct.pack(">I", ms)
        sock.sendall(slim_frame(b"strm", bytes(packed)))
        report("strm-p pause %d ms" % ms)

    def send_strm_u(self, sock, jiffies=0):
        packed = bytearray()
        packed += b"u"
        packed += b"\x00" * 13
        packed += struct.pack(">I", jiffies)
        sock.sendall(slim_frame(b"strm", bytes(packed)))
        report("strm-u resume jiffies=%d" % jiffies)

    def send_aude(self, sock, enable):
        sock.sendall(slim_frame(b"aude", bytes([1 if enable else 0, 0])))
        report("aude enable=%d" % (1 if enable else 0))

    def send_cont(self, sock, metaint=0):
        packed = bytearray()
        packed += struct.pack(">I", metaint)
        packed += b"\x00"  # loop
        sock.sendall(slim_frame(b"cont", bytes(packed)))
        report("cont sent metaint=%d" % metaint)

    def send_audg(self, sock, pct):
        audg = bytearray()
        audg += struct.pack(">I", 0)   # old gain L
        audg += struct.pack(">I", 0)   # old gain R
        audg += b"\x01"                # adjust
        audg += b"\x00"                # preamp
        audg += struct.pack(">I", int(pct * 65536.0))
        audg += struct.pack(">I", int(pct * 65536.0))
        sock.sendall(slim_frame(b"audg", audg))
        report("audg volume=%d%%" % pct)

    def player(self, sock):
        state = "helo"
        volume_done = False
        stop_done = False
        last_report = 0.0
        t0 = time.time()
        try:
            while True:
                frame = read_frame(sock)
                if frame is None:
                    report("player disconnected")
                    return
                opcode = frame[:4]
                payload = frame[4:]
                if opcode == b"HELO":
                    device_id = payload[0]
                    revision = payload[1]
                    self.mac = ":".join("%02x" % b for b in payload[2:8])
                    caps = payload[36:].decode(errors="replace")
                    report(
                        "HELO device_id=%d revision=%d mac=%s caps=<%s>"
                        % (device_id, revision, self.mac, caps)
                    )
                    self.tracks_sent = 1
                    if not self.silent:
                        self.send_strm_start(sock)
                elif opcode == b"RESP":
                    report("RESP %s" % payload[:96].decode(errors="replace"))
                    if self.codc_codec:
                        self.send_codc(sock, self.codc_codec)
                    if self.autostart >= 2:
                        self.send_cont(sock)
                elif opcode == b"STAT":
                    event = payload[:4].decode(errors="replace")
                    jiffies = struct.unpack(">I", payload[25:29])[0]
                    elapsed = struct.unpack(">I", payload[43:47])[0]
                    received = struct.unpack(">I", payload[19:23])[0]
                    stream_full = struct.unpack(">I", payload[11:15])[0]
                    out_full = struct.unpack(">I", payload[33:37])[0]
                    now = time.time()
                    if event != "STMt" or now - last_report >= 1.0:
                        report(
                            "STAT %s jiffies=%d elapsed_ms=%d br=%d stream_full=%d out_full=%d"
                            % (event, jiffies, elapsed, received, stream_full, out_full)
                        )
                        last_report = now
                    if self.queue_tracks and event == "STMd":
                        # Decoder ready: LMS advances the playlist here.
                        report("STMd received (track %d/%d)" % (self.tracks_sent, self.queue_tracks))
                        if self.tracks_sent < self.queue_tracks:
                            self.tracks_sent += 1
                            report("advancing to track %d" % self.tracks_sent)
                            self.send_strm_start(sock)
                    if self.volume_pct > 0 and not volume_done and event in ("STMs",) and elapsed >= 0:
                        volume_done = True
                        self.send_audg(sock, self.volume_pct)
                    if self.skip_ms and not self.skip_sent and event == "STMs":
                        self.skip_sent = True
                        self.send_strm_a(sock, self.skip_ms)
                    if self.send_aude_off and not self.aude_sent and event == "STMs":
                        self.aude_sent = True
                        self.send_aude(sock, False)
                    if (self.pause_after and not self.pause_sent
                            and elapsed >= int(self.pause_after * 1000)):
                        self.pause_sent = True
                        self.pause_time = time.time()
                        self.send_strm_p(sock, 0)
                    if (self.pause_sent and not self.resume_sent
                            and time.time() - self.pause_time >= self.pause_for):
                        self.resume_sent = True
                        self.send_strm_u(sock, 0)
                    if self.stop_after > 0 and not stop_done and elapsed >= self.stop_after * 1000:
                        stop_done = True
                        report("sending strm-q after %.1fs" % self.stop_after)
                        sock.sendall(slim_frame(b"strm", b"q"))
                else:
                    report("unhandled opcode %s" % opcode)
        except (ConnectionResetError, BrokenPipeError):
            report("connection reset by player")

    def run(self):
        report("fake-lms tcp=%d http=%d" % (self.tcp_port, self.http_port))
        threading.Thread(target=self.handle_http, daemon=True).start()
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.tcp_port))
        srv.listen(4)
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=self.player, args=(conn,), daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description="fake LMS for squeeze2raop2 testing")
    parser.add_argument("--tcp-port", type=int, default=3483)
    parser.add_argument("--http-port", type=int, default=9000)
    parser.add_argument("--stream-seconds", type=float, default=8.0)
    parser.add_argument("--volume-pct", type=int, default=68)
    parser.add_argument("--no-volume", dest="volume_pct", action="store_const", const=0)
    parser.add_argument("--stop-after-sec", type=float, default=0.0)
    parser.add_argument(
        "--queue-tracks",
        type=int,
        default=0,
        help="serve N short tracks, advancing on the player's STMd (queue test)",
    )
    parser.add_argument("--autostart", type=int, default=1, help="strm-s autostart 0-3")
    parser.add_argument("--format", default="p", help="strm-s format: p/m/a/o/u/?")
    parser.add_argument("--replay-gain", type=int, default=0, help="strm-s replay gain (16.16)")
    parser.add_argument("--transition", type=int, default=0, help="strm-s transition type 0-4")
    parser.add_argument("--transition-secs", type=int, default=0, help="strm-s transition period")
    parser.add_argument("--skip-ms", type=int, default=0, help="send strm-a skip after STMs")
    parser.add_argument("--aude-off", action="store_true", help="send aude(0) after STMs")
    parser.add_argument("--codc", default=None, help="reply to RESP with this codc format")
    parser.add_argument("--stall-after-sec", type=float, default=0.0,
                        help="stop sending HTTP body after N s (STMo test)")
    parser.add_argument("--silent", action="store_true",
                        help="send nothing after HELO (watchdog test)")
    parser.add_argument("--pause-after-sec", type=float, default=0.0,
                        help="send strm-p after N s of playback")
    parser.add_argument("--pause-for-sec", type=float, default=0.0,
                        help="resume with strm-u after N s paused")
    parser.add_argument("--aac-fixture", default=None,
                        help="serve this AAC file as format 'a' (ADTS/MP4)")
    parser.add_argument("--container", default="adts", choices=["adts", "mp4"],
                        help="AAC transport, mapped to the pcm sample-size code")
    parser.add_argument("--aac-reps", type=int, default=0,
                        help="repeat the AAC fixture this many times (0 = auto)")
    parser.add_argument("--ogg-fixture", default=None,
                        help="serve this Ogg Vorbis file as format 'o'")
    parser.add_argument("--opus-fixture", default=None,
                        help="serve this Ogg Opus file as format 'u'")
    args = parser.parse_args()
    lms = FakeLms(
        args.tcp_port,
        args.http_port,
        args.stream_seconds,
        args.volume_pct,
        args.stop_after_sec,
        args.queue_tracks,
        autostart=args.autostart,
        fmt=args.format,
        replay_gain=args.replay_gain,
        transition=args.transition,
        transition_secs=args.transition_secs,
        skip_ms=args.skip_ms,
        send_aude_off=args.aude_off,
        codc_codec=args.codc,
        stall_after=args.stall_after_sec,
        silent=args.silent,
        pause_after=args.pause_after_sec,
        pause_for=args.pause_for_sec,
        aac_fixture=args.aac_fixture,
        container=args.container,
        aac_reps=args.aac_reps,
        ogg_fixture=args.ogg_fixture,
        opus_fixture=args.opus_fixture,
    )
    lms.run()


if __name__ == "__main__":
    main()

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
    def __init__(self, tcp_port, http_port, stream_seconds, volume_pct, stop_after):
        self.tcp_port = tcp_port
        self.http_port = http_port
        self.stream_seconds = stream_seconds
        self.volume_pct = volume_pct
        self.stop_after = stop_after
        self.mac = ""

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
                audio = build_stream_bytes(self.stream_seconds + 60.0, 44100)
                conn.sendall(
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: audio/wav\r\n"
                    b"Server: fake-lms\r\n"
                    b"\r\n"
                )
                sent = 0
                t0 = time.time()
                while sent < len(audio):
                    chunk = audio[sent:sent + 44100 * 4]
                    conn.sendall(chunk)
                    sent += len(chunk)
                    target = sent / (44100.0 * 4)
                    elapsed = time.time() - t0
                    if target > elapsed:
                        time.sleep(min(target - elapsed, 0.5))
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass

    def send_strm_start(self, sock):
        packed = bytearray()
        packed += b"s" * 0
        packed += b"s"      # command: start
        packed += b"1"      # autostart
        packed += b"p"      # format: pcm
        packed += b"1"      # sample size: 16 bit
        packed += b"3"      # sample rate: 44.1 kHz
        packed += b"2"      # channels: stereo
        packed += b"?"      # endianness unknown (wav container)
        packed += b"\x02"   # threshold 2 KB
        packed += b"\x00"   # spdif
        packed += b"\x00"   # transition period
        packed += b"\x30"   # transition type '0'
        packed += b"\x40"   # flags: stream without restart
        packed += b"\x10"   # output threshold 1.0s
        packed += b"\x00"   # reserved/slaves
        packed += struct.pack(">I", 0)          # replay gain
        packed += struct.pack(">H", self.http_port)
        packed += bytes([127, 0, 0, 1])         # server ip
        packed += ("GET /stream.mp3?player=%s HTTP/1.0\r\n" % self.mac.replace(":", "%3A")).encode()
        packed += b"\r\n"
        sock.sendall(slim_frame(b"strm", bytes(packed)))
        report("strm-s sent (autostart=1, pcm, wav container)")

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
                    self.send_strm_start(sock)
                elif opcode == b"RESP":
                    report("RESP %s" % payload[:96].decode(errors="replace"))
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
                    if self.volume_pct > 0 and not volume_done and event in ("STMs",) and elapsed >= 0:
                        volume_done = True
                        self.send_audg(sock, self.volume_pct)
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
    args = parser.parse_args()
    lms = FakeLms(args.tcp_port, args.http_port, args.stream_seconds, args.volume_pct, args.stop_after_sec)
    lms.run()


if __name__ == "__main__":
    main()

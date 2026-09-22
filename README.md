# squeeze2raop2

A Squeezebox-to-AirPlay bridge: it registers as a squeezelite-style player with
[Logitech Media Server](https://github.com/Logitech/slimserver) and streams
LMS's audio to AirPlay receivers — primary target: the **HomePod**, over native
**AirPlay 2**.

## Why this exists

This project was written out of frustration: the iOS 27 update on the HomePod
broke AirPlay 1 streaming with third-party libraries, so none of the classic
RAOP-era bridges could reach it anymore. Rather than patching an AirPlay 1
emitter to survive, this is a new bridge that speaks AirPlay 2 (HAP
pair-verify, encrypted RTSP/RTP) from scratch — and it was written using
**GLM-5.3-Flash**.

## What it does

- Registers with LMS as a squeezelite-class player (slimproto: HELO caps,
  STAT semantics, pause/stop/unpause, autostart)
- Discovers AirPlay receivers via mDNS; prefers AirPlay 2, falls back to
  classic RAOP (`protocol = ap1`), or targets a fixed receiver (`target`)
- Streams LMS audio through the vendored AirPlay 2 sender (pair-verify,
  encrypted control + timing channels, retransmit)
- Volume: LMS `audg` → slider percent → configurable dB anchors
  (`volume-map`) → receiver `SET_PARAMETER volume`, including mute; volume is
  applied mid-stream, remembered across session restarts, and can be pinned
  (`volume = fixed`, `volume-pct`)
- ICY in-band metadata from streams → DMAP now-playing on the receiver,
  pass-through `META` to LMS
- Clean stop/pause: FLUSH + ring drain so the receiver doesn't keep playing
  its jitter-buffer tail
- Scheduled latency tuning (`latency-ms`, default 500 ms)
- Honest codec caps (`pcm,mp3`) so LMS transcodes everything else (FLAC etc.)
  losslessly on the LAN

## Build & run

Linux, CMake ≥ 3.16, C++20:

```sh
cmake -B build
cmake --build build -j
ctest --test-dir build
# vendored sender's own tests (excluded from all):
cmake --build build --target raop_core_tests raop_loop_tests
```

Example: bridge a HomePod as an LMS player named `Kueche15`. All behavior lives
in one INI-style config/state file (default `./squeeze2raop2.conf`); the only
CLI flag is `--config <file>`.

```ini
[global]
lms       = 192.168.1.10:3483   # omit for UDP discovery on 3483
discovery = on
log       = debug

[default]                       # inherited by every player, then overridden
volume-map = -30:1, -23:16, -15:50, 0:100
latency-ms = 500

[player "Kueche15"]
id     = 542a1b5cc9e2            # optional 12-hex mDNS device id
mac    = aa:22:53:7d:3c:01       # virtual MAC (assigned if omitted)
target = 192.168.1.157:7000      # fixed receiver => static player
```

```sh
./build/squeeze2raop2 --config squeeze2raop2.conf
```

A section spawns at startup when it is user-authored; discovered devices are
matched by `id`, then virtual `mac`, then name, and (with
`auto-register = on`) get an `auto = true` section so their MAC and pairing
credentials persist. The program rewrites only the machine-managed `mac` and
`creds` keys, preserving your comments and layout. A legacy
`squeeze2raop2.state` is imported automatically on first run.

## The volume chain

LMS encodes the slider as a 16.16 linear gain in `audg`; the bridge inverts
LMS's dB curve to recover the slider percent, maps it through the
`volume-map` piecewise-linear dB anchors (default `-30:1, -23:16, -15:50,
0:100`, i.e. slider 16 = −23 dB, 50 = −15 dB, 100 = full scale, 0 = mute),
and pushes the result as `SET_PARAMETER volume`. The AirPlay protocol floor
is −30 dBFS with −144 as the mute sentinel; the anchors keep the quiet end
of the slider in audible territory instead of collapsing into the floor.
The anchor design mirrors philippe44's squeeze2raop `VolumeMapping`.

## Credits

- **akustikrausch** — author of airplay2-sender-cpp, the AirPlay 2 sender
  core this bridge streams through
- **philippe44** — squeezelite-as-library and the LMS-Raop/squeeze2raop
  bridges: the bridge-idea forefather; the volume anchor system follows his
  design
- **Triode (Adrian Smith)** — squeezelite, the slimproto protocol reference
- **The Logitech Media Server team** — LMS itself, plus the slimserver Perl
  sources that document the audg/volume encoding and STAT semantics
- **The pyatv project** — AirPlay sender protocol reference (dB volume
  scale, FLUSH form, user-agent/digest auth details)
- **The owntone project** — receiver-side behavior parity reference
- **GLM-5.3-Flash** — wrote this code

## Third-party dependencies used

Vendored under `third_party/`; each keeps its own license.

| Dependency | Role | License |
|---|---|---|
| [airplay2-sender-cpp](https://github.com/HansKeksnascher/airplay2-sender-cpp) (fork) | AirPlay 2 sender core: HAP pair-verify, encrypted RTSP/RTP, retransmit | Apache-2.0 |
| [apple-oss-distributions/mDNSResponder](https://github.com/apple-oss-distributions/mDNSResponder) (mDNSPosix) | embedded mDNS browse/announce | Apache-2.0 |
| [minimp3](https://github.com/lieff/minimp3) | single-header MP3 decoder | CC0 1.0 |
| [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | TLS/crypto underneath the sender | Apache-2.0 |

## Limitations

- AirPlay volume can only go down to −30 dBFS; below that the receiver
  mutes. Digital (bridge-side) attenuation is not implemented.
- Codecs are `pcm` and `mp3` only; everything else relies on LMS transcoding.
- Developed and tested against one HomePod and one LMS 9.1 server.
- mDNS is Linux-only (mDNSPosix).
- LMS's HTTP JSON-RPC endpoint can return empty replies on some versions
  (the CLI port 9090 is unaffected).

## License

MIT — see [LICENSE](LICENSE). Vendored code in `third_party/` keeps its own
licenses (see the table above).
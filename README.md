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
pair-verify, encrypted RTSP/RTP) from scratch, using **GLM-5.3-Flash** and
**DeepSeek V4.1 Flash**.

## Features

- **LMS player** — registers as a squeezelite-class player (slimproto HELO
  caps, STAT semantics, pause/stop/unpause, autostart) with squeezelite-parity
  extensions: `aude` power on/off, `codc` codec negotiation, `setd` rename
  (persisted), `strm a` skip-ahead, replay gain, `STMl`/`STMo`/`DSCO` STAT
  events, and fade in/out (`strm` transition types 2/3/4).
- **Discovery and targeting** — browses AirPlay receivers via mDNS, prefers
  AirPlay 2 and falls back to classic RAOP (`protocol = ap1`), or targets a
  fixed receiver (`target`) for a static player.
- **AirPlay 2 streaming** — streams through the vendored sender (HAP
  pair-verify, encrypted control and timing channels, retransmit).
- **Volume control** — LMS `audg` slider → configurable dB anchors
  (`volume-map`) → receiver `SET_PARAMETER volume`, including mute; applied
  mid-stream, remembered across session restarts, and pinnable
  (`volume = fixed`, `volume-pct`).
- **Now-playing metadata** — ICY in-band metadata from streams → DMAP on the
  receiver, passed through as `META` to LMS.
- **Clean transport** — stop/pause sends FLUSH and drains the ring so the
  receiver doesn't play out its jitter-buffer tail; `latency-ms` tunes the
  scheduled AirPlay latency (default 500 ms).
- **Resilient control link** — an LMS-silence watchdog (`server-timeout-ms`)
  reconnects a dead control connection instead of waiting on TCP keepalive.
- **Source-stall watchdog** — a stream whose HTTP source delivers nothing for
  `source-timeout-ms` is ended (`DSCO` + `STMu`) so LMS re-issues it, instead
  of hanging on a half-open socket forever.
- **Honest codec caps** — advertises `pcm,mp3,aac,ogg,ops`, so LMS transcodes
  FLAC and everything else losslessly on the LAN; AAC radio and `.m4a` files
  stream natively (ADTS + MP4 demux, AAC-LC/HE-AAC), Ogg Vorbis (`.ogg`,
  Vorbis radio) and Ogg Opus (`ops`, Opus radio) stream natively too — all
  decoded in-process. A codec announced via `codc` that the bridge can't decode
  is rejected with `STMn`.
- **Direct HTTPS radio** — advertises `CanHTTPS=1`, so LMS hands over a
  `strm s` with the direct `https://` URL and the `0x20` TLS flag instead of
  proxying the stream through the server. The bridge fetches it over TLS
  (vendored mbedTLS), verifying the station certificate against the system
  trust store by default; `tls-verify = off` or `tls-ca = <path>` override it.
  When TLS setup fails the cap is withheld, so LMS keeps proxying.

## Build and run

### Build

Linux, CMake ≥ 3.16, C++20:

```sh
git submodule update --init --recursive   # sender, mDNSResponder, mbedtls, minimp3, stb, libxaac, libogg, libopus
cmake -B build
cmake --build build -j
```

Tests:

```sh
ctest --test-dir build -L unit         # unit cases (one runner, --filter subsets)
ctest --test-dir build -L integration  # bridge + fake-LMS scenarios
# vendored sender's own tests (excluded from all):
cmake --build build --target raop_core_tests raop_loop_tests
```

Unit tests self-register with `SQ2_TEST(suite, name)` and run in one process
(`./build/test/squeeze2raop2_tests`); add `--list` or `--filter <suite>` to
select cases. Integration scenarios live in `test/integration/` and assert on
the bridge's behaviour, exiting non-zero on failure.

### Configure

All behavior lives in one INI-style config/state file (default
`./squeeze2raop2.conf`). CLI flags are `--config <file>`, `-V/--version` and
`-h/--help`.

```ini
[global]
lms       = 192.168.1.10:3483   # omit for UDP discovery on 3483
discovery = on
log       = debug
# server-timeout-ms = 35000     # reconnect after this much LMS silence
# source-timeout-ms = 15000     # end a stream silent for this long (0 = off)
# tls-verify = on               # verify direct https stream certificates
# tls-ca = /etc/ssl/certs/ca-certificates.crt   # override the trust store

[default]                       # inherited by every player, then overridden
volume-map = -30:1, -23:16, -15:50, 0:100
latency-ms = 500

[player "Kueche15"]
id     = 542a1b5cc9e2            # optional 12-hex mDNS device id
mac    = aa:22:53:7d:3c:01       # virtual MAC (assigned if omitted)
target = 192.168.1.157:7000      # fixed receiver => static player
```

A section spawns at startup when it is user-authored; discovered devices are
matched by `id`, then virtual `mac`, then name, and (with
`auto-register = on`) get an `auto = true` section so their MAC and pairing
credentials persist. The program rewrites only the machine-managed `mac`,
`creds` and `name` keys, preserving your comments and layout. A legacy
`squeeze2raop2.state` is imported automatically on first run.

### Run

```sh
./build/squeeze2raop2 --config squeeze2raop2.conf
./build/squeeze2raop2 --version
```

## Runtime requirements

The dynamically linked binary needs only the GNU C/C++ runtime. The vendored
sender, mbedTLS, mDNSResponder, minimp3, stb_vorbis, libxaac, libogg and libopus
are all linked in statically, so there is **no** Avahi/D-Bus, ALSA/PulseAudio or
external TLS library dependency. The `-static` artifact is a musl build, so it
carries no runtime libraries at all. Any decoder can be dropped at configure
time: `-DSQUEEZE2RAOP2_WITH_AAC=OFF`, `-DSQUEEZE2RAOP2_WITH_OGG=OFF` or
`-DSQUEEZE2RAOP2_WITH_OPUS=OFF`; the HELO caps then advertise only the codecs
that remain built in. Direct HTTPS streaming can be dropped with
`-DSQUEEZE2RAOP2_WITH_HTTPS=OFF` (the caps then omit `CanHTTPS=1`).

One piece of *data* is needed for direct HTTPS: a CA bundle to verify station
certificates (the system store is autodetected, or set `[global] tls-ca`). With
no bundle found the bridge logs a warning and withholds `CanHTTPS=1`, so LMS
keeps proxying; `[global] tls-verify = off` disables verification instead.

| Artifact | Needs at runtime |
|---|---|
| `squeeze2raop2-linux-x86_64` | `libc6` (glibc), `libstdc++6`, `libgcc-s1`, `libm.so.6`, `/lib64/ld-linux-x86-64.so.2` |
| `squeeze2raop2-linux-x86_64-static` | none (Linux kernel only) |

Each release ships a `.requires.txt` with the exact `GLIBC_*`/`GLIBCXX_*`/
`CXXABI_*` symbol floor the dynamic binary was built against; compare it with
your distro if you run something older than the Ubuntu 24.04 build host.
`tools/runtime-deps.sh <binary>` reproduces the report locally.

## Third-party dependencies

Vendored as git submodules under `third_party/`; each keeps its own license.

| Dependency | Role | License |
|---|---|---|
| [airplay2-sender-cpp](https://github.com/HansKeksnascher/airplay2-sender-cpp) (fork) | AirPlay 2 sender core: HAP pair-verify, encrypted RTSP/RTP, retransmit | Apache-2.0 |
| [apple-oss-distributions/mDNSResponder](https://github.com/apple-oss-distributions/mDNSResponder) (mDNSPosix) | embedded mDNS browse/announce | Apache-2.0 |
| [minimp3](https://github.com/lieff/minimp3) | single-header MP3 decoder | CC0 1.0 |
| [stb](https://github.com/nothings/stb) (stb_vorbis) | Ogg Vorbis decoder (push-data) | MIT / public domain |
| [libogg](https://github.com/xiph/ogg) | Ogg page demuxer for Opus | BSD-3-Clause |
| [libopus](https://github.com/xiph/opus) | Ogg Opus decoder | BSD-3-Clause |
| [libxaac](https://github.com/ittiam-systems/libxaac) | AAC-LC/HE-AAC decoder (ADTS + MP4 demux) | Apache-2.0 |
| [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | crypto underneath the sender + the direct-stream HTTPS client | Apache-2.0 |

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
- **GLM-5.3-Flash** and **DeepSeek V4.1 Flash** — wrote this code

## License

MIT — see [LICENSE](LICENSE). Vendored code in `third_party/` keeps its own
licenses (see the table above).
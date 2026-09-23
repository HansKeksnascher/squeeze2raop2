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
- Squeezelite-parity slimproto: `aude` power on/off, `codc` codec negotiation,
  `setd` rename (persisted as a machine-managed key), `strm a` skip-ahead,
  replay gain, `STMl`/`STMo`/`DSCO` STAT events, and fade in/out (`strm`
  transition types 2/3/4)
- LMS-silence watchdog (`server-timeout-ms`) so a dead control connection is
  reconnected instead of waiting on TCP keepalive
- Scheduled latency tuning (`latency-ms`, default 500 ms)
- Honest codec caps (`pcm,mp3`) so LMS transcodes everything else (FLAC etc.)
  losslessly on the LAN

## Build & run

Linux, CMake ≥ 3.16, C++20:

```sh
git submodule update --init --recursive   # sender, mDNSResponder, minimp3, mbedtls
cmake -B build
cmake --build build -j
ctest --test-dir build -L unit         # unit cases (one runner, --filter subsets)
ctest --test-dir build -L integration  # bridge + fake-LMS scenarios
# vendored sender's own tests (excluded from all):
cmake --build build --target raop_core_tests raop_loop_tests
```

Unit tests self-register with `SQ2_TEST(suite, name)` and run in one process
(`./build/test/squeeze2raop2_tests`); add `--list` or `--filter <suite>` to
select cases. Integration scenarios live in `test/integration/` and assert on
the bridge's behaviour, exiting non-zero on failure.

### Versioning

The version is derived from git: a reachable `vX.Y.Z` tag wins, otherwise
`1.0.0+g<sha>`; a dirty tree appends `-dirty`. It is compiled in and used for
the startup log, the `Firmware=` string LMS shows in player settings, and the
CLI:

```sh
./build/squeeze2raop2 --version        # squeeze2raop2 1.0.0+g1234abc
```

Tarball builds without `.git` can force it with `-DSQUEEZE2RAOP2_VERSION=...`.

### Release and static builds

`-DSQUEEZE2RAOP2_DIST=ON` names the binary `squeeze2raop2-<os>-<arch>` (so a
cross build gets e.g. `squeeze2raop2-linux-aarch64`); `-DSQUEEZE2RAOP2_STATIC=ON`
links it fully static and appends `-static`.

The released `-static` artifact is built against **musl**, so it carries no
runtime libraries and resolves names itself (no NSS/`nsswitch.conf`). A glibc
`-static` build looks equivalent but still dlopens NSS modules at runtime — on
systemd distros that can crash inside `libnss_resolve`/`libnss_myhostname` — so
use the musl toolchain:

```sh
tools/fetch-musl-toolchain.sh ~/musl-toolchain
MUSL_TOOLCHAIN_ROOT=~/musl-toolchain/x86-64--musl--stable-2026.08-1 \
  cmake -B build-musl -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-musl-x86_64.cmake \
        -DSQUEEZE2RAOP2_DIST=ON -DSQUEEZE2RAOP2_STATIC=ON
cmake --build build-musl -j
```

CI and the release pipeline live in `.github/workflows/`.

### Runtime requirements

The dynamically linked release binary needs only the GNU C/C++ runtime. The
vendored sender, mbedTLS, mDNSResponder and minimp3 are all linked in
statically, so there is **no** Avahi/D-Bus, ALSA/PulseAudio or TLS dependency:

| Artifact | Needs at runtime |
|---|---|
| `squeeze2raop2-linux-x86_64` | `libc6` (glibc), `libstdc++6`, `libgcc-s1`, `libm.so.6`, `/lib64/ld-linux-x86-64.so.2` |
| `squeeze2raop2-linux-x86_64-static` | none (Linux kernel only) |

Every release ships a `.requires.txt` with the exact `GLIBC_*`/`GLIBCXX_*`/
`CXXABI_*` symbol floor the dynamic binary was built against; compare it with
your distro if you run something older than the Ubuntu 24.04 build host.
`tools/runtime-deps.sh <binary>` reproduces the report locally.

Example: bridge a HomePod as an LMS player named `Kueche15`. All behavior lives
in one INI-style config/state file (default `./squeeze2raop2.conf`); the CLI
flags are `--config <file>`, `-V/--version` and `-h/--help`.

```ini
[global]
lms       = 192.168.1.10:3483   # omit for UDP discovery on 3483
discovery = on
log       = debug
# server-timeout-ms = 35000     # reconnect after this much LMS silence

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
credentials persist. The program rewrites only the machine-managed `mac`,
`creds` and `name` keys, preserving your comments and layout. A legacy
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

Vendored as git submodules under `third_party/`; each keeps its own license.

| Dependency | Role | License |
|---|---|---|
| [airplay2-sender-cpp](https://github.com/HansKeksnascher/airplay2-sender-cpp) (fork) | AirPlay 2 sender core: HAP pair-verify, encrypted RTSP/RTP, retransmit | Apache-2.0 |
| [apple-oss-distributions/mDNSResponder](https://github.com/apple-oss-distributions/mDNSResponder) (mDNSPosix) | embedded mDNS browse/announce | Apache-2.0 |
| [minimp3](https://github.com/lieff/minimp3) | single-header MP3 decoder | CC0 1.0 |
| [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) | TLS/crypto underneath the sender | Apache-2.0 |

## Limitations

- AirPlay volume can only go down to −30 dBFS; below that the receiver
  mutes. The LMS volume slider therefore stays receiver-side; replay gain and
  fades are the only bridge-side (digital) gain stages.
- Crossfade (`strm` transition type 1) is not implemented: each track is torn
  down or flushed between songs, so fade in/out only.
- Codecs are `pcm` and `mp3` only; everything else relies on LMS transcoding
  (or, for an unknown codec announced via `codc`, is rejected with `STMn`).
- Developed and tested against one HomePod and one LMS 9.1 server.
- mDNS is Linux-only (mDNSPosix).
- LMS's HTTP JSON-RPC endpoint can return empty replies on some versions
  (the CLI port 9090 is unaffected).

## License

MIT — see [LICENSE](LICENSE). Vendored code in `third_party/` keeps its own
licenses (see the table above).
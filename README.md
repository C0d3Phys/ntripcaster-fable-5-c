<div align="center">

# NtripCaster

**A from-scratch NTRIP v1/v2 caster in C17 — epoll thread pool, lock-free relay, hashed auth.**

[![Language](https://img.shields.io/badge/language-C17-00599C?style=for-the-badge&logo=c&logoColor=white)](src/)
[![Build](https://img.shields.io/badge/build-CMake%203.20%2B-informational?style=for-the-badge&logo=cmake&logoColor=white)](CMakeLists.txt)
[![License](https://img.shields.io/badge/license-MIT-green?style=for-the-badge)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%2F%20WSL-lightgrey?style=for-the-badge&logo=linux&logoColor=white)](build.sh)
[![Protocol](https://img.shields.io/badge/protocol-NTRIP%20v1%20%7C%20v2-orange?style=for-the-badge)](docs/)
[![Sanitizers](https://img.shields.io/badge/verified%20with-ASan%20%7C%20UBSan-critical?style=for-the-badge)](docs/)

[Español](README.es.md) · [Docs](docs/) · [Real-hardware guide](docs/GUIA_GNSS_REAL.md)

</div>

---

## What is this

NtripCaster is an [NTRIP](https://en.wikipedia.org/wiki/Networked_Transport_of_RTCM_via_Internet_Protocol) v1/v2 caster — the relay server that sits between a GNSS base station streaming RTCM3 corrections and the rover receivers that consume them for RTK/PPK positioning. It's written from zero in C17, with no framework and no legacy codebase underneath: a single-purpose caster built around three core design decisions.

- **epoll, edge-triggered, `EPOLLONESHOT` thread pool.** One accept loop, N workers, no per-connection thread. A fd is only ever owned by one worker at a time — no locking needed on the hot path.
- **Lock-free SPMC ring buffer per mountpoint.** The source writes once; every subscribed rover reads at its own pace from the same 256 KB circular buffer, zero-copy. A rover that falls behind more than the buffer size gets disconnected instead of stalling the source.
- **Incremental RTCM3 decoder for observability, decoupled from the relay path.** Bytes reach clients exactly as they arrived — corrupted or not. Decoding runs in parallel purely to extract station coordinates, receiver metadata, and per-message-type integrity stats for the logs.

This is an actively developed systems project, not a weekend script. It has a written design-review trail (see [`docs/`](docs/)), stress tests under AddressSanitizer/UBSan, and a real end-to-end CTest scenario — not just "it compiled."

## Features

| Area | What's in |
|---|---|
| **Protocol** | NTRIP v1 (`SOURCE`, plain `GET`) and v2 (`POST`, chunked-aware), sourcetable generation, Basic Auth |
| **RTCM3** | Incremental frame parser (handles frames split across reads), CRC24Q validation, decode of station coords (1005/1006), antenna (1007/1008), receiver info (1033), MSM (1071–1137), ephemerides (GPS/GLONASS/BeiDou/QZSS/Galileo), legacy GPS/GLONASS obs (1001–1012) |
| **Concurrency** | epoll ET + `EPOLLONESHOT` worker pool, CAS-based capacity reservation (no TOCTOU on `max_sources`/`max_clients`), atomic per-mountpoint stats |
| **Auth** | PBKDF2-HMAC-SHA256 password hashing (vendored SHA-256, constant-time compare, zero external deps), fail-closed by design, hot-reload via `SIGHUP` — no restart, no dropped connections |
| **Reliability** | Idle/handshake timeout sweeps, ordered shutdown (no fd leaks, no dangling `conn_t` on `SIGTERM`), separated queued-vs-sent byte counters |
| **Observability** | Full request headers logged (credentials masked) at every SOURCE/GET, periodic per-mountpoint throughput + corruption stats, structured `warn`/`error`/`info` levels throughout |
| **Testing** | End-to-end CTest scenario (synthetic RTCM3 → relay → caster → rover → byte-for-byte compare), manual validation against a real production NTRIP source ([use-snip.com](https://www.use-snip.com/)) |

## Architecture

```
                 ┌─────────────────────────────────────────────┐
                 │                io_engine                    │
                 │  epoll (ET) + EPOLLONESHOT + worker pool     │
                 │  accept loop → work queue → N worker threads │
                 └───────────────┬───────────────┬─────────────┘
                                 │               │
                     dispatch_source()   dispatch_client()
                                 │               │
                                 ▼               │
                 ┌─────────────────────────┐     │
   SOURCE ──────▶│  broker (auth, limits)  │     │
   (RTCM3 push)  └────────────┬────────────┘     │
                              ▼                  │
                 ┌─────────────────────────┐     │
                 │      mountpoint_t       │     │
                 │  ┌───────────────────┐  │     │
                 │  │ ring_buffer (SPMC)│──┼─────┘
                 │  │  256 KB, lock-free│  │   rb_read() per rover
                 │  └───────────────────┘  │
                 │  incremental RTCM3       │
                 │  decoder (coords, msg    │
                 │  type stats, CRC24Q)     │
                 └─────────────────────────┘
                              │
                              ▼
                  GET /mount ─── rover 1, rover 2, ... N
```

Every connection is a `conn_t` owned by exactly one worker at a time (enforced by `EPOLLONESHOT`); mountpoints and their ring buffers live in a flat pre-allocated registry. Auth, capacity limits, and stats all live in the `broker`, which the protocol layer (`src/protocol/`) never touches directly.

## Getting started

### Build

```bash
git clone https://github.com/C0d3Phys/ntripcaster-fable-5-c.git
cd ntripcaster-fable-5-c
./build.sh 1.0.0          # configures + builds into build-1.0.0/
```

Requires CMake ≥ 3.20, a C17 compiler (GCC/Clang), and `pthread`. No other external dependencies — `inih`, `uthash`, and SHA-256 are vendored in [`vendor/`](vendor/).

### Configure

```bash
cp conf/ntripcaster.conf.example conf/ntripcaster.conf
```

Passwords are stored hashed — **never** in plain text — using PBKDF2-HMAC-SHA256. Generate a hash for a mountpoint or rover credential:

```bash
echo -n "your-real-password" | ./build-1.0.0/src/ntripcaster --hash-password
# -> pbkdf2-sha256$100000$<salt>$<hash>
```

Paste the resulting string into `conf/ntripcaster.conf`:

```ini
[source]
BASE1 = pbkdf2-sha256$100000$...        # your GNSS base / relay pushes here

[client:BASE1]
rover1 = pbkdf2-sha256$100000$...       # rover 1's Basic Auth credential
rover2 = pbkdf2-sha256$100000$...
```

The plaintext password is still what your receiver or NTRIP client sends over the wire — that's the protocol. The hash is only how the caster stores it at rest, so the config file itself isn't a credential leak if it ever gets exposed.

### Run

```bash
./build-1.0.0/src/ntripcaster 2101 conf/ntripcaster.conf ntripcaster.log
```

Reload credentials or caster identity without dropping connections:

```bash
kill -HUP $(pidof ntripcaster)
```

See [`docs/GUIA_GNSS_REAL.md`](docs/GUIA_GNSS_REAL.md) for a full walkthrough connecting a real GNSS base + rovers.

## Testing

```bash
ctest --test-dir build-1.0.0 --output-on-failure
```

The CTest suite spins up a real caster on an ephemeral port, feeds it synthetic RTCM3 via a relay, consumes it with a rover client, and byte-compares what went in against what came out — no mocks. Development builds are additionally validated under `-fsanitize=address,undefined` for the concurrency-sensitive paths (ring buffer, capacity reservation, connection lifecycle).

## Project layout

```
src/
├── core/       io_engine (epoll/threads), broker, mountpoint, ring_buffer, logger, config
├── gnss/       RTCM3 frame parsing + decoding (MSM, ephemerides, station/receiver info)
├── protocol/   NTRIP v1/v2 handshakes, sourcetable, auth + password hashing
└── main.c
test/
├── tools/      standalone relay/rover/compare CLIs used by the E2E harness
├── helpers/    synthetic RTCM3 generator, bash test helpers
└── cases/      CTest scenarios
vendor/         inih, uthash, sha256 (all vendored, zero package manager deps)
docs/           design docs, decision records, hardware integration guide
```

## Status

This project is under active development. Core protocol, concurrency, and auth hardening are done and tested; a larger connection-ownership refactor (reference-counted `conn_t` lifecycle under heavy concurrent churn) is the next major piece of work. See [`docs/`](docs/) for the full improvement log and design rationale behind every non-trivial decision in this codebase.

## License

[MIT](LICENSE) — see the `LICENSE` file for details.

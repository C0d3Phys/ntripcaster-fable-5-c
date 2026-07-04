# NTRIP integration tools

These programs exercise a real end-to-end path without embedding passwords:

```text
external caster -> ntrip_source_relay -> local ntripcaster -> ntrip_rover_client
```

Build from the repository root:

```bash
cmake -S . -B build-test
cmake --build build-test -j
```

Start the local caster:

```bash
./build-test/src/ntripcaster
```

Relay an external mountpoint into local `BASE1`:

```bash
export UPSTREAM_PASS='external-password'
export LOCAL_SOURCE_PASS='passbase123'
./build-test/test/tools/ntrip_source_relay \
  caster.example 2101 REMOTE_MOUNT external_user \
  127.0.0.1 2101 BASE1
```

Consume and validate local `BASE1` for 30 seconds:

```bash
export LOCAL_ROVER_PASS='passrover1'
./build-test/test/tools/ntrip_rover_client \
  127.0.0.1 2101 BASE1 rover1 30 capture.rtcm3
```

The rover exits successfully only when it received bytes, decoded at least one
valid RTCM3 frame, and found no skipped/corrupt bytes. Omit the duration and
capture path to run until `Ctrl+C` without writing a file.

These are diagnostic executables, not daemons. Reconnection and supervision
should be added only if the relay is later promoted to a production service.

# NTRIP integration tools

These programs exercise a real end-to-end path without embedding passwords:

```text
external caster -> ntrip_source_relay -> local ntripcaster -> ntrip_rover_client
```

Copy and edit the shared configuration:

```bash
cp test/tools/ntrip_tools.conf.example test/tools/ntrip_tools.conf
chmod 600 test/tools/ntrip_tools.conf
```

The same file configures the external caster, the local source and the rover.
Build from the repository root:

```bash
cmake -S . -B build-test
cmake --build build-test -j
```

Start the local caster:

```bash
./build-test/src/ntripcaster
```

Relay the configured external mountpoint into local `BASE1`:

```bash
./build-test/test/tools/ntrip_source_relay --config test/tools/ntrip_tools.conf
```

Consume and validate local `BASE1` for 30 seconds:

```bash
./build-test/test/tools/ntrip_rover_client --config test/tools/ntrip_tools.conf
```

The relay creates a UTC session directory and the rover joins it:

```text
capture_rtcm3_bin_UTC/20260704T183000Z/
  relay_rtcm3.bin
  relay.meta
  rover_rtcm3.bin
  rover.meta
```

Compare both captures after stopping them:

```bash
./build-test/test/tools/ntrip_capture_compare \
  capture_rtcm3_bin_UTC/20260704T183000Z
```

The comparison aligns complete RTCM3 frames byte-for-byte. Frames before the
rover connected and after it stopped are reported separately and are not
counted as packet loss. `lost_inside` measures frames present at the relay but
missing from the rover inside their common interval.

The rover exits successfully only when it received bytes, decoded at least one
valid RTCM3 frame, and found no skipped/corrupt bytes. Omit the duration and
capture path to run until `Ctrl+C` without writing a file.

The original command-line mode remains available for temporary overrides; run
either executable with `--help` to see it.

These are diagnostic executables, not daemons. Reconnection and supervision
should be added only if the relay is later promoted to a production service.

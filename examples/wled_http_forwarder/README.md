# sentry-micro + WLED — HTTP-forwarder example

A WLED usermod that recovers a crash from the coredump partition, builds a Sentry
envelope, and pushes it over plain HTTP to a forwarder — no TLS on the device. The
forwarder is a separate, stateless process (`http_forwarder.py`, in this directory) that
relays plain HTTP to real HTTPS ingest.

## Layout

This directory holds only what's specific to this example:

```
examples/wled_http_forwarder/
├── platformio_override.ini      # copy into your WLED checkout, see below
├── http_forwarder.py            # run this alongside the device, see below
└── usermods/sentry/
    ├── library.json
    └── usermod_sentry.cpp       # the whole usermod
```

WLED itself is **not** vendored here — clone it separately and point
`platformio_override.ini` at this repo. Nothing in the WLED checkout needs to change:
current WLED usermods self-register via `REGISTER_USERMOD` (a linker-section trick),
and `custom_usermods` supports referencing a library by absolute path
(`LibName = symlink:///abs/path`), so `usermod_sentry.cpp` can live here, in
version control, rather than being copied into WLED's own `usermods/` folder.

## Requirements

- An ESP32 board with at least 4MB of flash.
- WLED, cloned separately (see Setup).
- A Sentry DSN for a test project.
- Python 3 to run `http_forwarder.py`, on a machine reachable from the board's LAN.

## Setup

1. Clone WLED locally.

2. Copy `platformio_override.ini` from this directory into the root of that WLED
   checkout, and edit the absolute paths (`symlink://...`) to point at wherever this
   repo lives on your machine. Leave the `platform =` and `-UWLED_ENABLE_DMX_INPUT`
   lines alone — see "Platform override" below before touching either.

3. Run the forwarder (from this directory) with your project's real DSN:

   ```
   python3 http_forwarder.py --dsn https://abc123@o456.ingest.us.sentry.io/789
   ```

   It prints the DSN to put on the device, e.g.:

   ```
   device SENTRY_DSN:
       http://abc123@192.168.1.42:8080/789
   ```

   Same public key and project id as your real DSN, host:port swapped for wherever the
   forwarder is listening. Set that as `SENTRY_DSN` in `platformio_override.ini`'s
   `build_flags`. See `usermod_sentry.cpp`'s header comment for why this works — the
   ingest URL is built purely from the DSN, so nothing on the device needs to know the
   real ingest host or region.

4. From the WLED checkout:

   ```
   pio run -e sentry_poc -t upload -t monitor
   ```

## Trying it

- Boot log should show WiFi joining and `[sentry]` init/boot-report lines (needs
  `-D WLED_DEBUG` in `build_flags`, already set in the provided override — WLED's
  `DEBUG_PRINTLN`/`DEBUG_PRINTF` are no-ops without it). Look for
  `[sentry] coredump: supported=1 available=..` specifically — `supported=0` means this
  build's Arduino core doesn't have coredump-to-flash on at all; see "Platform override"
  below.
- `curl http://<device-ip>/sentry/crash` forces a crash 500ms later (long enough for the
  HTTP response to flush before the reboot).
- On the next boot, the serial console shows the recovered exception/backtrace, and the
  forwarder's own log shows the incoming POST and the upstream response.
- `sentry-micro`'s NVS-backed buffer holds the envelope if the forwarder is unreachable,
  and retries it on the next `sentry_flush()` tick (every 30s while something is
  buffered) — kill the forwarder before triggering a crash to see this, then restart it
  and watch the next tick deliver it.

## Platform override

`extends = env:esp32dev` inherits WLED's own platform pin — a Tasmota fork of
`platform-espressif32`, Arduino core 3.3.8 / IDF 5.5.5. That build's prebuilt Arduino
core ships with `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE` (not `TO_FLASH`), confirmed across
**all 28** chip/flash-mode variants it distributes — not a classic-ESP32 quirk, a
package-wide default. That's baked into precompiled `libespcoredump.a`/`libesp_system.a`;
no build flag reaches it. `platformio_override.ini` overrides `platform =` to
`espressif32@7.0.1` specifically to get a build where coredump-to-flash is actually on.

That override has a side effect: WLED's shared `[esp32_idf_V5]` build flags
unconditionally set `-D WLED_ENABLE_DMX_INPUT`, pulling a fork of `esp_dmx` written
against IDF 5.x's `dmx_config_t` shape. The older platform's DMX library doesn't have
that shape, so `dmx_input.cpp` fails to compile unless DMX input is turned back off —
hence `-UWLED_ENABLE_DMX_INPUT` in the override. Nothing to do with coredump; a casualty
of picking an older platform on purpose. If you need DMX input for your own testing,
you'll need a different resolution to this same conflict.

## Known issues and limitations

- **Symbolication**: this example captures a real backtrace (multiple correct frames,
  exception type, task, reset reason) but does not upload a matching debug file, so
  Sentry cannot yet resolve frame addresses to function names.
- **Stale coredump partitions**: if you're switching between partition tables, boards, or
  platform pins on the same physical chip, do a full flash erase first. A `coredump`
  partition at a given flash offset in one layout can coincide with unrelated leftover
  bytes from a completely different layout used earlier, and still pass
  `esp_core_dump_image_check()` — producing a spurious, internally-inconsistent report
  (e.g. an exception type paired with a reset reason that doesn't cohere with it).
  `esptool.py erase_flash` before your first real test avoids the ambiguity entirely.
- **Platform choice**: `espressif32@7.0.1` gets coredump-to-flash working but is
  noticeably older than what WLED ships by default, uses more flash for the same build
  (roughly 88% vs. 73% of the app partition), and conflicts with DMX input as noted
  above. A production usermod needs either an upstream `platform-espressif32` build with
  both coredump-to-flash and current WLED API compatibility, or a self-built Arduino core
  with a custom sdkconfig.

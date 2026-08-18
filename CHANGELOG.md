# Changelog

## Unreleased

### Various fixes & improvements

- Initial project scaffold: portable core, ESP32 device context, transport interface.
- DSN parsing with ingest-URL and `X-Sentry-Auth` construction, covered by host unit tests.
- Org id recovered from an `o<digits>.` DSN host, with an `Options::org_id` override for
  self-hosted instances and custom ingest domains.
- `Transport::send()` returns a `Response` struct (result, HTTP status, retry-after) rather
  than a bare enum, so rate-limit handling has somewhere to land.
- The whole public API is C, with C++ as an inline wrapper — usable from an ESP-IDF
  component with no C++ runtime.
- Fixed-buffer JSON writer and envelope/event builder, with no allocation anywhere on the
  reporting path. Verified on a host and byte-for-byte on an ESP32-PICO-D4.
- `scripts/env_secrets.py`: WiFi credentials and DSN come from `SENTRY_MICRO_*` environment
  variables, so nothing sensitive needs to exist in a file.
- Device context: chip model/revision/cores, eFuse-derived device id, flash size, heap,
  ESP-IDF version, and `esp_reset_reason()` mapped to a stable set of Sentry reset reasons.
- PlatformIO packaging (`library.json`, `library.properties`) plus a reference partition
  table that includes the `coredump` partition the stock Arduino tables omit.
- `wifi_basic` example, building on ESP32, ESP32-S2, ESP32-S3, ESP32-C3 and (opt-in) ESP32-C6.
- `WiFiTransport`: HTTPS POST straight to ingest, refusing any host but the DSN's (exact
  match, no redirects), with `Retry-After` / `X-Sentry-Rate-Limits` translated into a
  backoff the core can act on.
- `SerialTransport` plus `scripts/serial_relay.py`: relay envelopes through the host on the
  other end of the USB cable, for devices with no usable network. Same architecture as the
  planned BLE relay, over a link that is trivial to debug.
- Base64 encoder in the portable core, streamed in fixed chunks so memory use does not grow
  with envelope size.
- Offline ring buffer (`core/sentry_buffer.*`): portable ring policy over a pluggable
  storage vtable, so undelivered envelopes survive a reboot. Host-tested including wrapped
  recovery, corrupt metadata, and failed writes. NVS storage and SDK wiring still to come.
- NVS and filesystem (LittleFS/SPIFFS/SD) storage backends, plus `sentry_enable_buffering()`
  and `sentry_flush()`. Undelivered envelopes persist and are retried; server-supplied
  `Retry-After` is honoured.
- Symbolication pipeline: `scripts/release.sh` builds, stamps a GNU build-id into the ELF as
  a non-ALLOC note, and uploads debug files with `sentry-cli`. Events now carry
  `debug_meta.images[]` with the matching `code_id`/`debug_id`.
- Core dump reading: crashes are recovered on the next boot and reported as a Sentry
  `exception` with a stacktrace, symbolicated server-side against the uploaded ELF.
- Relay protocol (`core/sentry_relay.*`): a generic, non-Sentry-specific framing for "perform
  this HTTP request for me" over any link that carries short packets both ways. Host-tested
  including reassembly at every awkward chunk size.
- `RelayTransport`: reports through a companion app over BLE, serial or anything else, with no
  Sentry knowledge required on the app side. Proven against ChromaBay's NimBLE service; the
  app-side reference implementation is ~200 lines including the ingest-host whitelist.
- Events carry the real `image_addr`/`image_size` read from the linked ELF, which is what
  symbolication actually resolves against; `release.sh` now builds in two passes to obtain
  them.
- Debug images carry a `code_file`, so frames show the firmware name instead of `<unknown>`
  in the module column.
- Generic relay protocol (`core/sentry_relay.*`) and `RelayTransport`: binary, chunked
  framing for packet links with a small MTU, so a companion app over BLE (or any bidirectional
  link) can perform the request. Host-whitelisted like the other transports.
- Core dump reading now has a real RISC-V path; the C-series report `mcause`/`mtval` and no
  precomputed backtrace, which is a different structure rather than different field names.
- Events say whether the backtrace is complete: a `backtrace` tag plus `frames_captured` and
  `backtrace_truncated` in the esp32 context, so a short RISC-V trace cannot be mistaken for
  a whole stack.
- Verified on hardware over WiFi (ESP32-PICO-D4, M5Stack): boot events and crash reports
  both delivered straight to ingest, and the offline buffer survives a reboot — two
  envelopes written with no network were recovered from NVS on the next boot and flushed
  once the radio came up.
- `image_size` now stops at the end of the last executable segment rather than the last
  loadable one. On ESP32 a 16-byte RTC segment at 0x50000200 stretched the reported extent
  to 281 MB of mostly-unoccupied address space, which invited Sentry to attribute a stray
  address to this image and resolve it to a confidently wrong symbol.
- `scripts/release.sh` iterates to a fixpoint instead of assuming two passes. Baking the
  measurements in changes them — on Xtensa a large constant moves from a two-byte `movi.n`
  to a four-byte literal — so the build now re-measures and re-bakes until an ELF matches
  the numbers it was built with, and fails loudly if it never settles.
- Fixed the "what networks can I see?" diagnostic, which failed every single time it ran.
  It asked for a 1000ms dwell per channel, but Arduino's `scanNetworks()` waits a
  hard-coded 10s for a scan that visits ~13 channels; it always timed out and returned -2,
  and the failed call left `WIFI_SCANNING_BIT` set so every retry then reported -1 forever.
- `-D SENTRY_CRASH_BUTTON_PIN=<gpio>` crashes the example on a button press, so the
  crash -> reboot -> report -> symbolicate loop is repeatable without a one-minute reflash
  each time. `-D SENTRY_DEMO_SCAN=1` runs the network scan on every boot.
- `image_size` is emitted as a JSON number rather than a hex string. Relay types it as a
  plain unsigned integer — unlike `image_addr` right beside it, which is an address and does
  take `"0x..."` — so every event we ever sent had the field discarded with
  `expected an unsigned integer`. Ingest still answered 200 and the issue still rendered;
  the only trace was an "Event Processing Errors" panel on the event page.

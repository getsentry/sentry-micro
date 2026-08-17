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

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
- `-D SENTRY_DEMO_SCAN=1` runs the network scan on every boot, not just after a failed
  connect — a diagnostic nobody can exercise is how the one above came to be broken.
- `image_size` is emitted as a JSON number rather than a hex string. Relay types it as a
  plain unsigned integer — unlike `image_addr` right beside it, which is an address and does
  take `"0x..."` — so every event we ever sent had the field discarded with
  `expected an unsigned integer`. Ingest still answered 200 and the issue still rendered;
  the only trace was an "Event Processing Errors" panel on the event page.
- A crash report is tagged `crashed: true` whenever it carries a core dump, not only when
  the boot that sent it was itself a crash. The two normally agree; they come apart when the
  dump outlives that boot (delivery failed, or the board was power-cycled or reflashed
  first), and the old logic tagged those `crashed: false` — so an alert on `crashed:true`
  would skip exactly the crashes that were hardest to deliver.
- Reusable GitHub Actions for the release chain: `upload-debug-files` builds every variant
  a project declares, stamps each with its own build-id, uploads the debug files and
  verifies the result; `list-environments` turns `platformio.ini` into a job matrix.
  Symbolication coverage is now derived from the file that defines the variants, so adding
  a board fails the release until someone decides whether it ships — rather than shipping
  a variant whose users silently get raw addresses.
- Debug-file uploads are now checked rather than assumed. `objcopy` exits 0 whether or not
  the build-id note landed, and `sentry-cli` reports "matched 0 files" as success, so
  `scripts/check_debug_file.py` reads the stamped ELF back with the same library Sentry
  uses, the upload passes `--id ... --require-all`, and `--wait` makes the server's verdict
  visible instead of only the fact that the bytes were accepted.
- A release that produces two variants with the same `debug_id` now fails. Sentry would
  resolve one binary's addresses against the other and print confidently wrong frames,
  which is worse than no symbolication because nothing about the result looks wrong.
- `release.sh` gained `--no-wait`, `--no-sources` and `--json-summary`, and accepts an
  absolute `--project-dir` so it can run against a project in another repository.
- `sentry_capture_message(level, message)` and `sentry::capture_message(...)`: report a
  non-crash event in one call instead of hand-assembling one. It prepares the event, frames
  the envelope on the caller's stack, sends it, and buffers it for retry if there is no
  route — the same path everything else takes. `examples/wifi_basic` now uses it for the
  boot report, which took fifteen lines before.
- A client-side throttle on captured messages, because the API is easy to call from a loop
  and a loop runs forever: the same message at the same level is sent at most once per
  `message_repeat_window_ms` (default 10s), and everything else is capped at
  `max_messages_per_minute` (default 10). Repeats are checked first so a stuck message
  cannot spend the budget a different one needed, and the window runs from the last message
  *sent*, so a caller faster than the window still reports once per window instead of going
  quiet. Crash reports bypass it entirely. `sentry_suppressed_count()` reports what it ate;
  `-D SENTRY_DEMO_FLOOD=1` demonstrates it on a board.
- The "built but never exercised on hardware" list in README.md was stale: it still claimed
  `WiFiTransport` had never delivered an event and that the buffer's power-cycle path was
  host-tested only. Both were confirmed on hardware, and ONBOARDING.md already said so.
- The capture throttle's repeat window now runs from when the previous identical message
  *finished* being sent, not from when the call began. Found on hardware: `capture_message()`
  sends inline, and a send with no route blocks for the transport's timeout — 15.2 s
  measured, against `SerialTransport`'s 15 s default and a 10 s window. Timed from the start
  of the call the window had always already elapsed by the time the caller looped round, so
  no repeat was ever suppressed: a twenty-iteration loop produced seventeen events and
  evicted thirteen envelopes from the offline buffer. Timing from completion makes the rule
  hold however slow the transport is.
- `-D SENTRY_MICRO_WIFI_TLS=0` compiles the HTTPS branch out of `WiFiTransport`, so nothing
  references `WiFiClientSecure` and the linker drops mbedTLS. A runtime `if (is_https)` kept
  it linked whether or not a device ever took that branch, which made the no-TLS variant
  nominal rather than real. Measured on `esp32dev`: the flash image falls from 941,248 to
  813,840 bytes — 124 KB — plus 904 bytes of RAM. Such a build refuses an `https://` ingest
  URL with `SEND_REJECTED` rather than downgrading it, since sending the envelope in clear
  would put the DSN's write key on the wire, and `set_ca_cert()` is compiled out so pinning
  a certificate in a no-TLS build is a compile error rather than a silent no-op.
- Trace context: `sentry_trace_adopt()` / `sentry_trace_start()` / `sentry_trace_release()`,
  with the `sentry-trace` and `baggage` headers parsed in `core/` and events carrying a
  `trace` context — and a `replay` context when the calling app had a replay running, so a
  device crash links to the recording of the interaction that caused it. A trace is scoped to
  one unit of work rather than to the device's lifetime: holding the last-seen trace would
  attach an unrelated panic hours later to that interaction, which renders exactly like a
  real causal link. Malformed headers are rejected whole rather than partly believed, since
  adopting the readable half of a garbled header joins a trace that does not exist.
- The active trace survives a panic in RTC slow memory (`RTC_NOINIT_ATTR`), which is cleared
  on power-on but preserved across a software reset — so a crash reported on the next boot
  still carries the trace it happened inside, while a cold boot correctly carries none.
- Releases now carry their commits. `release.sh` runs `sentry-cli releases set-commits --auto
  --ignore-missing` and `releases finalize` after registering, so an issue shows what changed
  between builds rather than only that a build broke — which on a device you cannot attach a
  debugger to is usually the more useful half. Run from the firmware's own repository, not
  sentry-micro's, so the Action associates the adopter's commits and not this SDK's.
  `--no-commits` / `set-commits: false` opts out.
- A shallow checkout no longer quietly associates one commit. `actions/checkout` defaults to
  `fetch-depth: 1`, so this is the normal state of a CI workspace — and one commit is not
  "fewer commits", it is a release claiming the build contains a single change when it
  contains a hundred, rendering identically to the truth. The Action now deepens the
  checkout itself, and `release.sh` refuses the release if it is still shallow, before
  building rather than after, so a doomed run costs a second and leaves no half-populated
  release behind.
- `sentry_trace_adopt()` now checks the incoming `sentry-org_id` baggage key against the
  device's own (`Options::org_id`, resolved the same way it always has been) before joining
  a trace, and refuses one from a different organization rather than mixing telemetry across
  accounts. A device or caller that does not know its own org id is not treated as a
  mismatch by default; set `Options::strict_trace_continuation` to require both sides to
  agree instead. A refused trace does not fail the call — the device becomes the head of a
  fresh trace instead, the same as `sentry_trace_start()`, since the request is still real
  even though it is not part of the caller's trace.
- `sentry-sample_rand` is likewise parsed out of incoming `baggage` and carried on the trace
  context. Not consumed yet — there is nothing here to sample without spans — but captured
  now so a later increment does not have to revisit where the Dynamic Sampling Context comes
  from.
- A crash recovered from the previous boot keeps the trace it died inside, even when a new
  trace is adopted first. The app that comes to collect a crash report connects and offers a
  new trace in the same breath, so sharing one slot meant the panic was attached to the
  connection that came to fetch it — a wrong link that renders identically to a right one.
  The recovered trace now lives apart from the active one and `sentry_event_attach_coredump()`
  uses it, so the ordering of "adopt" and "report the last boot" stops mattering. Found by
  the first real integration, not in review.
- The persisted trace slot is cleared at `sentry_init()` once recovered, so a later panic on
  a boot where nothing was ever adopted no longer resurrects the previous crash's trace and
  claims the two are the same incident.
- Corrected a false claim in the SDK's own documentation: `Options::release` did **not** have
  to match what `sentry-cli` uploaded debug files under, contrary to what `sentry_micro.h`
  asserted in bold, and what `sentry_envelope.h` and README.md repeated. Symbolication
  matches `debug_id` derived from the GNU build-id; `sentry-cli debug-files upload` takes no
  release argument at all. JavaScript source maps *are* release-scoped, which is where the
  assumption comes from. Reported by the first external integrator, who had architected
  around the constraint because the header stated it as the field's contract.
- `scripts/debug_paths.py`: an opt-in PlatformIO pre-script that rewrites the build machine's
  absolute paths in the debug info to a placeholder rooted at the repository name, so a stack
  frame can be matched against a file in a commit from any machine, and no home directory
  ends up in a public firmware's debug files. Off by default, and the header explains why:
  `sentry-cli --include-sources` locates sources by the paths in the debug info, so a
  placeholder prefix costs the line of code shown beside each frame — measured going from
  "Resolved source code for 1 debug information file" to 0. For most projects a single code
  mapping on the CI path is the better trade, and the README now says so.

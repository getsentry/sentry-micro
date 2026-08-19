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
- Suspect commits need one code mapping in Sentry, from the CI build path to the repository
  root, because the compiler records absolute build paths that match nothing in a repo. A
  build-side alternative (`-ffile-prefix-map`) was tried and dropped: it makes paths machine
  independent but `sentry-cli --include-sources` then cannot find the sources, taking the
  line of code beside each frame with it — measured, 1 debug file resolved to 0.
- `exception.value` names the crashing task when there is no faulting address, instead of
  being omitted. Omitting it renders as the literal string "(No error message)" under the
  issue title — worse than the duplication that leaving it out was meant to avoid. The task
  name says something the type does not, and unlike the PC it is stable across builds, so it
  cannot split one issue per firmware in the no-frames grouping fallback. Caught on a real
  event by the ChromaBay integration.
- `scripts/sentry_config.py` derives the two Sentry project settings a native project needs
  and cannot get automatically — the code mapping and the stack trace rules — by reading the
  build paths back out of the ELF that was just built. `release.sh` prints them at the end of
  every upload, which is the only place the build prefix is knowable and, in CI, the only run
  where it is the right one. The rules invert the usual approach: the example firmware is
  built from 21 distinct directories and one is ours, so nothing is in-app until proven
  otherwise rather than trying to enumerate toolchains. Without that, suspect commits blames
  the first in-app frame and lands in `newlib`.
- `scripts/doctor.py` checks a release against the project it was uploaded to: debug files
  indexed under the id the firmware reports, the release existing, finalized and carrying
  commits, a code mapping whose stack root actually prefixes this ELF's build paths, and
  stack trace rules that keep toolchain frames out of your code. Checks the mapping against
  real paths rather than for existence, because one that is a directory off looks configured
  and matches nothing — and distinguishes a missing setting from a token that cannot see it.
- Transactions and spans: `sentry_transaction_start()` / `sentry_span_begin()` /
  `sentry_span_end()` / `sentry_transaction_end()`, emitted as a `transaction` envelope item
  on the trace the device already joined. The device stops being an error hanging off someone
  else's trace and becomes a participant with real durations.
- `sentry_span_set_measurement()` attaches numbers to a span, enriching the trace. Integers only: printf's float
  support is an opt-in linker flag on this target, and a `%f` in a build without it prints
  nothing at all, which would silently malform the JSON.
- Span durations come from the monotonic clock, so they are correct even when the wall clock
  is set part way through an operation. Only the transaction's position on the timeline needs
  a date, and if the device has never been told one the transaction is dropped rather than
  anchored to the epoch — a duration has no server-side substitute, unlike an event timestamp.
  Setting the clock stays the application's job.
- `sentry_json_kv_micros()` writes decimal seconds from an integer microsecond count, and
  `sentry_device_unix_time_us()` / `sentry_device_uptime_us()` expose the two clocks that
  feed it.
- Running out of span slots is counted and tagged `spans_dropped:true` rather than silently
  truncating, so a shortened trace can be found rather than mistaken for a simpler operation.
- The transaction is now caller-owned — `sentry_transaction_start(&txn, ...)` against a
  `sentry_transaction_t` you declare — instead of living in the SDK singleton. It was 3,248
  bytes of permanent RAM in every build whether or not spans were used, which is not what
  this SDK does anywhere else: an event is prepared into a caller's struct and a crash is
  read into a caller's `sentry_coredump_t`. Permanent RAM is back to zero.
- `SENTRY_MICRO_MAX_SPANS` defaults to 4 and `SENTRY_MICRO_MAX_SPAN_ATTRS` to 4, down from
  16 and 8. At 16 the transaction was 2,224 bytes and, with the 2 KB envelope buffer on top,
  peak stack in the finish call reached 4.2 KB — against the 8 KB Arduino gives the loop
  task, most of the rest of which a TLS handshake wants. Now 688 bytes and ~2.7 KB peak.
- The API follows sentry-native's verbs: `sentry_transaction_start()`,
  `sentry_transaction_start_child()`, `sentry_span_finish()`, `sentry_transaction_finish()`.
  Previously `start` and `begin` were mixed for no reason, and `span_measure()` collided with
  Sentry's separate "measurements" concept while actually setting an attribute. Ownership
  still differs deliberately — sentry-native hands back heap-managed handles and this SDK
  does not allocate.
- Corrected a wrong claim this SDK made in four places, including a public header: that
  Sentry's standalone metrics API "was withdrawn" and span attributes are what metrics are
  now. Application Metrics exist — `count` / `gauge` / `distribution`, JS SDKs 10.25.0 and
  above, their own `trace_metric` envelope item and their own explorer — and Sentry's own
  guidance separates them from span attributes explicitly: span metrics enrich existing
  traces and are subject to trace sampling, Application Metrics are for values that must not
  be sampled away. Telling an integrator that span attributes *are* the metrics story makes
  them stop looking. This SDK emits `event` and `transaction` items only; metrics are not
  implemented, and SDK-1418 now says so.

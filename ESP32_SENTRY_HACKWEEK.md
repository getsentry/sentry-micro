# Sentry for ESP32 / embedded — Hackweek proposal

**One-liner:** A drop-in Sentry SDK for ESP32 (Arduino/ESP-IDF) that captures crashes, reset
reasons, and telemetry on-device and delivers them to Sentry over *any* connectivity — WiFi
directly, or by handing a fully-formed Sentry envelope to a companion app that just relays bytes
to the internet.

Proven on real hardware: **ChromaBay**, an ESP32 BLE LED controller with a SvelteKit/Capacitor
app (already wired up with the Sentry browser SDK, so the app side of the relay is half-built).

---

## Why

Sentry has great SDKs for big platforms, but **embedded/IoT is a gap**. Hobbyist and prosumer
ESP32 projects crash in the field with no serial cable attached, and the maker has no idea why —
brownout? panic? watchdog? a null deref in their render loop? The data to answer that is *already
captured on the chip* (ELF coredump partition, `esp_reset_reason()`), it just never leaves.

There's a big, ready-made audience: **[WLED](https://github.com/wled/WLED)** (18k★, Arduino
framework, almost always on WiFi, huge installed base). It has a long, persistent tail of
hard-to-diagnose field reboots/crashes. **Signal (not a measured crash rate — keyword-matched
GitHub search, noisy; and there is no WLED telemetry, which is precisely the gap):** of ~3,700
issues ever filed, **~600 mention crash / reboot / brownout / watchdog** (56 currently open);
individual terms: "reboot" 436, "crash" 242, "exception" 151, "boot loop" 51, "guru meditation" 40.
Flagship threads run enormous: [#3685 "keeps rebooting after 0.14.1"](https://github.com/wled/WLED/issues/3685)
(184 comments), [#2932 reboots](https://github.com/wled/WLED/issues/2932) (168), and the forum has
dedicated multi-page threads ([ESP32 random reboots](https://wled.discourse.group/t/help-with-esp32-random-reboots/11638),
[boards reboot every 4–15 min](https://wled.discourse.group/t/multiple-esp32-boards-randomly-reboot/12503)).
The tell: these are *years* of GPIO/power/WiFi guesswork because the diagnostic data (reset reason,
ELF coredump) lives on the chip and never leaves. A usermod that ships symbolicated backtraces to
Sentry turns those guessing threads into a stack trace — and puts Sentry in front of the maker LED community.

Strategically: this extends Sentry into a segment it doesn't serve turnkey today, and it's a
*great demo* (crash a $5 microcontroller on stage, watch a symbolicated backtrace land in Sentry).

---

## Alignment with Sentry (prior art — this is the internal signal)

There is an open, upvoted request for exactly this:
**[getsentry/sentry-native#915 — "Feature Request: Support for ESP32 microcontroller"](https://github.com/getsentry/sentry-native/issues/915)**.
Key points from that thread:

- A Sentry collaborator (`supervacuus`) stated they will **not port `sentry-native` to
  microcontrollers** — it's built for "protected-mode" OSes (virtual memory, filesystem, threads,
  the infrastructure its API assumes). He explicitly floated that **"there might be a *sentry-micro*
  SDK at some point"** as the right vehicle for MCU/RTOS specifics. So Sentry's own framing is that
  this is a *separate* SDK, not a port. **This hackweek = a prototype of "sentry-micro."**
- Real demand: commenters cite *millions* of ESP32s on FreeRTOS and customers shipping ESP32 in
  end-customer hardware, already using Sentry elsewhere in their stack. The blocker per Sentry is
  that **"community and customer signal needs to be stronger"** — they explicitly asked people to
  make noise on the ticket and describe deployment context. A working hackweek demo answering #915
  *is* that signal, from inside the company.
- A community member (`KindDragon`) is already attempting a minimal ESP-IDF lib that POSTs a
  backtrace envelope for **server-side symbolication from uploaded debug files** — i.e. the exact
  architecture below — and has open questions nobody's answered yet. The hackweek can validate that
  path concretely (and collaborate).

Reference material to reuse rather than reinvent:
- **`getsentry/coredump-uploader`** — its event/stacktrace-from-coredump construction is a working
  template for the envelope shape; port that logic to C.
- The ingest protocol is SDK-agnostic: a plain `POST /api/<project>/envelope/` with an
  `X-Sentry-Auth` header works with no SDK at all — which is what makes the device-builds-the-envelope
  design viable on a chip with no room for a real SDK.

Note: this is **not** `sentry-native` and does **not** build on it — `sentry-native` targets
OS-class platforms. We interoperate with Sentry's *ingest + symbolication* (which is SDK-agnostic),
not with the native SDK's code.

## The core idea: the device builds the envelope; transport is a dumb pipe

The library owns all Sentry semantics; delivery is abstracted behind one interface:

```cpp
struct Transport { virtual int send(const char* url, const Headers& h, const uint8_t* body, size_t len) = 0; };
```

The device parses the DSN (`https://<key>@<host>/<project_id>`), builds the real **Sentry
envelope** (the ndjson that hits `POST https://<host>/api/<project>/envelope/` with an
`X-Sentry-Auth` header), and only needs *something* to deliver the bytes:

- **`WiFiTransport`** — POSTs directly to Sentry over HTTPS. Fully self-contained.
- **`RelayTransport`** — hands `(url, headers, body)` to a host callback; a companion app does a
  plain `fetch()` and returns the status. **The app has zero Sentry knowledge** — it's a generic
  "relay these bytes to this URL" service.

This is the whole trick: the same core works over BLE, WiFi, serial, even LoRa, and *any*
companion app supports it in ~20 lines. Auto-selection: **WiFi if connected → else a registered
relay → else buffer to flash and retry later.**

---

## Architecture

- **Core (portable, Sentry-specific)**
  - DSN parse; envelope + event JSON builder.
  - Native event shape for crashes: `platform:"native"`, `exception`/`threads` with
    `stacktrace.frames[].instruction_addr`, `debug_meta.images[]` carrying the firmware ELF's
    `debug_id` — so Sentry symbolicates server-side against uploaded debug files.
  - Crash sources: `esp_reset_reason()` at boot; `esp_core_dump_get_summary()` (crashing task +
    exception PC + a short backtrace of PCs — small enough to send without the whole dump).
  - **Offline ring buffer in NVS/flash** — essential for intermittently-connected (BLE) devices.
  - Rate-limit / dedup / backoff — honor `429` + `Retry-After` and envelope rate-limit headers;
    never re-send a boot-loop 500 times.
- **Transports** — `WiFiTransport`, `RelayTransport`, easy base class for custom ones.
- **Relay protocol** — a tiny *generic* BLE characteristic: device notifies a chunked
  `(url, headers, body)`, host POSTs, notifies status. Not Sentry-specific.
- **Companion relay reference impls** — web/Capacitor (have it), and ideally React Native, so any
  app can adopt the relay trivially.
- **Packaging** — PlatformIO/Arduino `library.json`, examples (wifi-direct, ble-relay), WLED usermod.

---

## The genuinely hard parts (scope honestly)

1. **Symbolication is a hard dependency.** Addresses are hex until the firmware's debug ELF is
   uploaded (`sentry-cli debug-files upload`) and the event carries a matching `debug_id`. Needs a
   GNU build-id in the ELF (`-Wl,--build-id`, which ESP-IDF/Arduino don't always emit) and a CI
   step per release. WLED's *matrix* of board/feature builds means every variant is a distinct
   debug-id to track — the fiddly bit that undercuts "out of the box." Honest promise: turnkey
   *event delivery*; symbolication is one documented CI step.
2. **On-device HTTPS (WiFi transport)** — TLS on a constrained chip: RAM + a CA root (or
   `setInsecure()`). Real, especially on ESP8266.
3. **Relay security** — the device asks the phone to POST arbitrary bytes to an arbitrary URL. The
   relay MUST whitelist the Sentry ingest host (derived from the configured DSN) so a buggy/rogue
   device can't use the phone as an open proxy. Non-negotiable.
4. **ESP8266 for full WLED coverage** — separate crash-capture path (no ESP-IDF coredump; a
   stack-dump-to-flash mechanism). Scope ESP32 first, note ESP8266 as an extension.

---

## Suggested week plan

- **Day 1 — Tier 1, cheap + high value.** `esp_reset_reason()` + heap telemetry → build an envelope
  → deliver over the ChromaBay BLE relay (app already runs the Sentry browser SDK). First events in
  Sentry, tagged by release + board + reset reason. Immediately answers real field mysteries.
- **Day 2 — Tier 2, actionable stacks.** Coredump summary → native frames; CI uploads `firmware.elf`
  debug info; get a *symbolicated* backtrace end-to-end. This is the money shot.
- **Day 3 — WiFiTransport + auto-select + offline buffer + rate-limiting.** The "real SDK" behaviors.
- **Day 4 — Extract to a standalone library** (core + transport interface) and stand up a **WLED
  usermod** using WiFiTransport; crash a WLED build and see it land.
- **Day 5 — Polish + demo + write-up.** Examples, README, the debug-file CI recipe, and a live
  "crash the chip → symbolicated event" demo.

---

## Padding / stretch ideas for a full week (pick to taste)

These make it a richer showcase and lean into Sentry-native features:

- **Release Health / Sessions.** Report device sessions (uptime, healthy vs crashed) → per-release
  **crash-free rate across a fleet of devices**. A very Sentry-native, very demo-able story for IoT.
- **Performance / tracing & metrics.** Send spans for render frame-time, BLE throughput, OTA
  duration; or metrics (FPS, min-free-heap over time, WiFi RSSI, **battery voltage**). "Errors +
  performance" on a microcontroller. (Battery voltage would have solved a real ChromaBay
  brownout mystery.)
- **Rich contexts/tags.** Chip model + revision, flash size, MAC-derived stable device id, board
  type, sdk/idf version, firmware version, WiFi signal — auto-attached to every event.
- **Breadcrumbs.** A flash/RTC ring buffer of handled events (OTA abort, config-parse fail, malloc
  fail, disconnects) attached for context before a crash.
- **Attachments.** Ship the full coredump as an attachment (offline `addr2line` even without server
  symbolication set up), or a snapshot of device state / the LED framebuffer.
- **Native-format research spike.** Investigate whether an ESP32 ELF coredump can be massaged into
  a format Sentry's native pipeline ingests directly — directly interesting to the processing team.
- **Reusable GitHub Action** for `sentry-cli debug-files upload` from a PlatformIO/Arduino build
  (handles the build-id + per-variant debug-id problem) — useful well beyond this project.
- **Multiple transports as a showcase.** WiFi + BLE relay + a serial transport (bench debugging) +
  maybe MQTT — proving the pluggable design.
- **DSN/config provisioning.** Set the DSN over BLE or a WiFi captive portal, so no rebuild to point
  at a project.
- **Footprint benchmarks.** Flash + RAM cost of the SDK, so adopters know the price of admission.
- **Host-side test harness.** A mock Sentry ingest endpoint to unit-test the envelope builder, plus
  a "force crash" command for repeatable demos.
- **Dev-marketing write-up.** "Crash reporting on a $5 microcontroller" blog post + the WLED angle —
  on-brand for Sentry and a natural hackweek deliverable.

---

## What success looks like

A maker adds a library + one CI line, and when their ESP32 panics in the field it shows up in
Sentry with a symbolicated backtrace, reset reason, and device context — over WiFi if they have it,
or relayed through their phone app if they don't. Bonus: it drops into WLED as a usermod, opening a
large ready-made audience.


---

## See Also

https://github.com/DrozmotiX/ioBroker.wled

https://github.com/pixel-heart/wledplus-releases

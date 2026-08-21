# sentry-micro + WLED — crash reporting relayed through the browser

A WLED usermod that recovers a crash from the coredump partition, builds a Sentry envelope,
and gets it to Sentry **without any TLS on the device and without anything to install or
operate**. The device serves a page; the browser that opens it performs the HTTPS POST.

<pre>
  ESP32 (WLED)                    your browser                     Sentry
  ────────────                    ────────────                     ──────
  panic → coredump partition
  next boot: decode, build
    envelope, queue in NVS
                     GET /sentry/relay/poll
                    ─────────────────────────►
                     BEGIN/DATA/END frames
                    ◄─────────────────────────
                                          fetch() POST /api/N/envelope/
                                         ──────────────────────────────►
                                                      200 OK
                                         ◄──────────────────────────────
                     POST /sentry/relay/status?req=1&result=0&http=200
                    ◄─────────────────────────
                     pop from NVS, done
</pre>

## Why it works this way

WLED has **zero** TLS anywhere in its tree — every internet-facing feature it ships is
plaintext — and Sentry's ingest is HTTPS-only. Adding mbedTLS to WLED to report a crash is
not really a byte problem (there is room on a 4MB board); it is that crash reporting is a
poor excuse to introduce the project's first TLS stack. There is also no WLED backend to
relay through: every wled.me property is static hosting.

So the device is never given a route to Sentry. It uses sentry-micro's **relay transport**
(`src/transport/sentry_transport_relay.hpp`), which chunks a finished HTTP request — URL,
two header values, envelope body — over any link that moves short packets both ways, and
waits for the host on the other end to report the status. That link is normally a phone over
BLE. Here it is a browser over HTTP polling. The frames are identical either way; the user's
browser already has a TLS stack, a CA store and an internet connection, and this borrows all
three for the length of one request.

What that buys:

- no TLS, no certificates, no CA bundle, no mbedTLS in the image
- nothing to install, run, or operate — no forwarder process, no broker, no backend
- the DSN is your project's **real** DSN, not one rewritten to point at a local relay

What it costs: **reports only leave the device while somebody has the relay page open.**
That is the right trade for a crash reporter. A crash is not discovered at crash time — it is
discovered on the *next* boot, out of the coredump partition — so it is written to a
flash-backed queue and delivered whenever a relay next attaches. For a device being actively
debugged, that is immediately.

> If you need reports to leave a device with nobody watching it, see
> [`../wled_http_forwarder`](../wled_http_forwarder) — same usermod shape, but the device
> POSTs plain HTTP to a small script you run on your own LAN, which relays to HTTPS. That one
> trades "somebody has the page open" for "somebody runs a process".

## Layout

WLED itself is **not** vendored here — clone it separately and point
`platformio_override.ini` at this repo. Nothing in the WLED checkout needs to change: current
WLED usermods self-register via `REGISTER_USERMOD` (a linker-section trick), and
`custom_usermods` supports referencing a library by absolute path
(`LibName = symlink:///abs/path`), so the usermod lives here, in version control, rather than
being copied into WLED's own `usermods/` folder.

```
examples/wled_browser_relay/
├── platformio_override.ini          # copy into your WLED checkout, see Setup
└── usermods/sentry/
    ├── library.json
    ├── usermod_sentry.cpp           # the usermod: SDK init, routes, delivery task
    └── sentry_relay_page.h          # the relay page (HTML/JS), served from PROGMEM
```

## Requirements

- An ESP32 board with at least 4MB of flash.
- WLED, cloned separately.
- A Sentry DSN for a test project.
- `sentry-cli` for the symbolication step (`brew install getsentry/tools/sentry-cli`).

## Setup

1. Clone WLED locally.

2. Point this repo at it and copy the override in:

   ```
   export SENTRY_MICRO_REPO=/path/to/sentry-micro
   cp platformio_override.ini /path/to/your/WLED/checkout/
   ```

3. Build and flash **with symbolication**, so backtraces resolve to functions and lines
   rather than staying hex forever:

   ```
   export SENTRY_MICRO_DSN='https://<key>@o<org>.ingest.us.sentry.io/<project>'
   scripts/release.sh -e sentry_relay -d /path/to/your/WLED/checkout --upload-firmware
   ```

   `release.sh` picks a build-id, compiles it in, stamps the matching debug-id into the ELF,
   and uploads that ELF's debug info to Sentry — see its header for why all four steps have
   to agree. A plain `pio run -e sentry_relay -t upload` also works and still reports
   crashes; the frames just stay as raw addresses.

   Unlike the forwarder example, `SENTRY_MICRO_DSN` here is your project's real DSN and does
   double duty correctly: it is both what the firmware reports to and what `sentry-cli`
   resolves the target project from.

   Two flags this project needs that `examples/wifi_basic` does not:

   - **`--no-commits`.** `release.sh` associates the project directory's commits with the
     release, and here that directory is *WLED's* repository, not yours — its commit history
     describes WLED's development, not what went into your build. (A shallow `git clone
     --depth N` of WLED is refused outright for the same reason, with a clearer message.)
   - **`-r <release>`.** Without it the release name is derived from `git describe` in the
     sentry-micro checkout, which has nothing to do with the firmware being built. Name it
     after WLED: `-r 'wled@17.0.0-dev'`.

   If `sentry-cli` reports *"Project not found"*, check which organization your auth token
   belongs to (`sentry-cli projects list`) before checking the project — an organization
   token silently overrides `--org`, so a token for the wrong org fails as a missing project.

4. Join the device to your WiFi the normal WLED way (its `WLED-AP` captive portal), then open:

   ```
   http://<device-ip>/sentry
   ```

   Leave that tab open.

## Trying it

With the relay page open:

- **"Send a test event"** captures a message event and relays it immediately. This is the
  fastest way to confirm DSN, network and relay are all working, before involving a panic.
- **"Crash the device"** panics it 500ms later (long enough for the HTTP response to flush).
  The device writes a coredump and reboots; the page shows it going unreachable and coming
  back. On the next boot the usermod decodes the coredump, queues the report in NVS, and the
  delivery task ships it through the page as soon as it re-attaches — which the page does on
  its very next poll.
- The serial console (`pio device monitor`) shows the same sequence from the device's side,
  including `[sentry] coredump: supported=1 available=..` at boot. `supported=0` means this
  build's Arduino core does not have coredump-to-flash on — see "Platform" below.
- Close the tab, crash it, and reopen the page later: the report is still there. That is the
  NVS-backed offline buffer, and it is the reason "only while a browser is watching" is an
  acceptable constraint rather than a hole.

Everything is inspectable with `curl`, which is deliberate:

```
curl http://<device-ip>/sentry/status        # DSN host, release, queue depth, coredump state
curl http://<device-ip>/sentry/relay/poll    # the queued frames, base64
```

## Routes the usermod adds

| Route | |
|---|---|
| `GET /sentry` | the relay page — open this and leave it open |
| `GET /sentry/status` | JSON the page renders, also useful on its own |
| `GET /sentry/relay/poll` | queued device→host frames, base64 |
| `POST /sentry/relay/status` | the host's verdict on one relayed request |
| `GET /sentry/test` | capture a message event |
| `GET /sentry/crash` | panic on purpose, 500ms from now |

## Design notes

**The HELLO frame is folded into the poll's query string.** On a BLE link the host announces
itself once, on connect; HTTP has no connect, and every poll is already the host saying "I am
here, I speak version N, my frames may be this big". More usefully, it makes reattachment
free — a device that panicked and rebooted has forgotten the host, and the next poll
re-announces it with no reconnect logic on either side. That is not an edge case; it is what
every crash does.

**Delivery runs on its own FreeRTOS task, not in `loop()`.** `RelayTransport::send()` blocks
until the browser answers or the timeout expires — a full round-trip through a page polling
once a second. Doing that from `loop()` would stall WLED's render for as long as it takes,
which for a lighting controller is the one cost that would actually disqualify the approach.
The task is priority 1 (below Arduino's loop task) and pinned to core 0.

**Frames are length-prefixed in the poll body.** HTTP hands the other end a byte stream with
no packet boundaries in it; a BLE notification supplies those for free. So the queue stores
`[len u16][frame]` repeated, and the page cuts them apart again.

**The poll body is base64.** Not aesthetics: this AsyncWebServer's response types are either a
`String` (not binary-safe — an envelope is full of bytes a `String` will not carry) or a
pointer it holds and reads later (also unsafe — the queue it points into is refilled by the
next send). A third more bytes buys a body with neither caveat, and one that prints legibly
under `curl`.

**Both ends whitelist the ingest host independently.** The device's `RelayTransport` refuses
any URL whose host is not the DSN's, and the page refuses anything that is not
`https://<the host /sentry/status reported>` with no embedded username. The page's check is
not defence against this device — the page came *from* the device — but against a firmware bug
turning the user's browser tab into an open proxy carrying their IP.

## Platform

`platformio_override.ini` switches from WLED's default platform to
`${esp32_idf_V5.platform_pioarduino}` — **WLED's own listed alternative**, not a third-party
pin. The reason is narrow and was re-checked against the current default rather than
inherited from an older note: WLED 2026.05.50 pins the Tasmota fork, whose prebuilt Arduino
core (3.3.8.260506) ships `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y` for **every one of the 12
chips it builds** — esp32, c2, c3, c5, c6, c61, h2, p4, p4_es, s2, s3, u. That is baked into
precompiled `libespcoredump.a` / `libesp_system.a`, so no build flag on our side turns it back
on. The pioarduino core has `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, and this build reports
`[sentry] coredump: supported=1` at boot because of it.

To check either for yourself:

```
grep COREDUMP ~/.platformio/packages/framework-arduinoespressif32*/**/esp32/sdkconfig
```

It also sets `board_build.partitions = tools/WLED_ESP32_4MB_256KB_FS.csv`, the one stock WLED
partition table for a 4MB board that reserves a `coredump` partition. `esp32dev`'s default has
none, and without one the panic handler has nowhere to write — the platform override is
necessary but not sufficient on its own.

### The flash budget

WLED notes that the pioarduino platform "needs 300kB extra flash", and on a 4MB board that is
the whole story of this build's size:

| | bytes | of 1,900,544 |
|---|---|---|
| this demo (stock usermods minus `audioreactive`) | 1,889,785 | 99.4% |
| the same build with `audioreactive` back in | 1,936,549 | 101.9% — does not fit |

So `custom_usermods` deliberately does **not** extend `${env:esp32dev.custom_usermods}`.
Audioreactive is the cheapest thing to give back on this board, because WLED's own
`platformio.ini` says it "crashes on classic esp32 (driver conflict related to legacy ADC)"
anyway. On an ESP32-S3 or an 8MB board there is no such squeeze and you can keep everything.

A production usermod would want neither override: it needs an upstream WLED platform with
coredump-to-flash enabled and a partition table that reserves 64K. Both are one-line changes
in WLED, and they are the conversation this demo exists to start.

## Using the device's own access point

An unconfigured WLED opens `WLED-AP` and serves everything from `4.3.2.1`. The relay page
works from there — the phone joins the device's AP and reaches Sentry over cellular, which is
a fair demonstration of the whole idea — but two things get in the way, and neither announces
itself:

- **WLED's captive portal hijacks DNS.** In AP mode it starts a wildcard DNS server
  (`dnsServer.start(53, "*", WiFi.softAPIP())`) that answers every query with its own address,
  so the ingest hostname resolves to the ESP32 and the POST goes to a device with no TLS
  stack. The browser reports a bare network error. The usermod stands that server down while
  a relay page is attached, and restores it when the page goes away.
- **A client that already cached the hijacked answer keeps using it.** Observed on iOS well
  past the 60-second TTL the record was served with. Toggling Wi-Fi off and on clears it;
  waiting does not reliably.

On a device joined to a normal network neither applies — there is no captive portal and no
wildcard DNS.

## Known issues and limitations

- **Reports flow only while the page is open.** By design, discussed above. The queue is 16
  NVS slots deep and survives reboots, so nothing is lost in the meantime; it is delayed.
- **Stale coredump partitions.** If you switch between partition tables, boards, or platform
  pins on the same physical chip, `esptool.py erase_flash` first. A `coredump` partition at a
  given offset in one layout can coincide with unrelated leftover bytes from a different
  layout and still pass `esp_core_dump_image_check()`, producing a spurious,
  internally-inconsistent report (e.g. an exception type paired with a reset reason that does
  not cohere with it).
- **The DSN is compiled in.** Fine for a demo; a released binary's DSN is extractable with
  `strings`, and a write key anyone can extract is a write key anyone can spend your quota
  with. A real usermod should take the DSN from WLED's own settings at runtime — which is
  also how ChromaBay (the other sentry-micro integration) does it.

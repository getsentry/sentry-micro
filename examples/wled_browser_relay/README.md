# sentry-micro + WLED — crash reporting relayed through the browser

A WLED usermod that recovers a crash from the coredump partition, builds a Sentry envelope,
and gets it to Sentry **without any TLS on the device, without anything to install or
operate, and without a page anybody has to open**. Open WLED the way you always would; the
reports go out behind the UI you are already looking at.

<pre>
  ESP32 (WLED)                  WLED's web UI                      Sentry
  ────────────                  ─────────────                      ──────
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
(`src/transport/sentry_transport_relay.hpp`), which chunks a finished HTTP request — URL, two
header values, envelope body — over any link that moves short packets both ways, and waits
for the host on the other end to report the status. That link is normally a phone over BLE.
Here it is WLED's own web UI. The frames are identical either way; the user's browser already
has a TLS stack, a CA store and an internet connection, and this borrows all three for the
length of one request.

What that buys:

- no TLS, no certificates, no CA bundle, no mbedTLS in the image
- nothing to install, run, or operate — no forwarder process, no broker, no backend
- the DSN is your project's **real** DSN, not one rewritten to point at a local relay
- **nothing for a user to do.** No page, no button, no setting. Anybody who opens WLED is
  relaying, and does not need to know it.

What it costs: **reports only leave the device while somebody has WLED open in a browser.**
That is the right trade for a crash reporter. A crash is not discovered at crash time — it is
discovered on the *next* boot, out of the coredump partition — so it is written to a
flash-backed queue and delivered whenever a relay next attaches. A device nobody has looked at
in a week keeps its reports until somebody does.

> If you need reports to leave a device with nobody watching it at all, see
> [`../wled_http_forwarder`](../wled_http_forwarder) — same usermod shape, but the device
> POSTs plain HTTP to a small script you run on your own LAN, which relays to HTTPS. That one
> trades "somebody has WLED open" for "somebody runs a process".

## What has to change in WLED

Two things, and it is worth being straight about the second:

1. **The usermod** — self-registers via `REGISTER_USERMOD`, referenced by absolute path from
   `custom_usermods`. Touches no WLED source file.
2. **~150 lines appended to `wled00/data/index.js`** (240 in the file, with its comments) —
   the host side of the relay protocol. A usermod cannot do this for itself: WLED gives
   usermods routes, Info-tab rows and a settings page, but no hook to run JavaScript in the
   main UI. (`/u`, the reserved "usermod custom web page", is compiled from
   `wled00/data/usermod.htm` — so it is a WLED-tree change too, and still a page somebody has
   to open.)

That second one is the actual ask of this demo: *a usermod, plus this much UI*. It is written
to be inert without the usermod — its first request is a feature check, and a WLED build
without the usermod answers 404 and it disables itself permanently. Carrying it costs one
request at page load and nothing after.

## Layout

WLED itself is **not** vendored here — clone it separately and point
`platformio_override.ini` at this repo. `custom_usermods` supports referencing a library by
absolute path (`LibName = symlink:///abs/path`), so the usermod lives here, in version
control, rather than being copied into WLED's `usermods/` folder.

```
examples/wled_browser_relay/
├── platformio_override.ini          # copy into your WLED checkout, see Setup
└── usermods/sentry/
    ├── library.json
    ├── usermod_sentry.cpp           # the usermod: SDK init, routes, delivery task
    └── wled_ui_relay.js             # the host side — append to WLED's index.js
```

## Requirements

- An ESP32 board with at least 4MB of flash.
- WLED, cloned separately.
- A Sentry DSN for a test project.
- `sentry-cli` for the symbolication step (`brew install getsentry/tools/sentry-cli`).

## Setup

1. Clone WLED locally.

2. Point this repo at it, copy the override in, and append the relay to WLED's UI:

   ```
   export SENTRY_MICRO_REPO=/path/to/sentry-micro
   cd /path/to/your/WLED/checkout

   cp $SENTRY_MICRO_REPO/examples/wled_browser_relay/platformio_override.ini .
   cat $SENTRY_MICRO_REPO/examples/wled_browser_relay/usermods/sentry/wled_ui_relay.js \
       >> wled00/data/index.js
   ```

   Appending, rather than shipping a positional patch: `index.js` is a flat script and the
   relay is a self-contained IIFE, so there is no context for a diff to lose when WLED's UI
   changes underneath it. `git checkout wled00/data/index.js` undoes it.

   The UI is rebuilt automatically — WLED's `pio-scripts/build_ui.py` runs `npm run build` on
   every compile, so `wled00/html_ui.h` picks the change up with no extra step. You need
   `node` on `PATH`; WLED requires it to build anyway. Note that touching `index.js`
   regenerates `html_ui.h` and therefore forces a **full** rebuild, not an incremental one.

3. Build and flash **with symbolication**, so backtraces resolve to functions and lines rather
   than staying hex forever:

   ```
   export SENTRY_MICRO_DSN='https://<key>@o<org>.ingest.us.sentry.io/<project>'
   scripts/release.sh -e sentry_relay -d /path/to/your/WLED/checkout \
       -r 'wled@17.0.0-dev' --no-commits --upload-firmware
   ```

   `release.sh` picks a build-id, compiles it in, stamps the matching debug-id into the ELF,
   and uploads that ELF's debug info to Sentry — see its header for why all four steps have to
   agree. A plain `pio run -e sentry_relay -t upload` also works and still reports crashes; the
   frames just stay as raw addresses.

   Two flags this project needs that `examples/wifi_basic` does not:

   - **`--no-commits`.** `release.sh` associates the project directory's commits with the
     release, and here that directory is *WLED's* repository — its history describes WLED's
     development, not what went into your build. (A shallow `git clone --depth N` of WLED is
     refused outright for the same reason, with a clearer message.)
   - **`-r <release>`.** Without it the release name comes from `git describe` in the
     sentry-micro checkout, which has nothing to do with the firmware being built.

   Re-flashing changed code under a release name you already used is a trap: the build-id is
   derived from the release name, so the new firmware claims a debug-id whose debug file
   describes the *old* binary, and Sentry will confidently symbolicate against it. Bump the
   release on every flash you intend to read backtraces from.

   If `sentry-cli` reports *"Project not found"*, check which organization your auth token
   belongs to (`sentry-cli projects list`) before checking the project — an organization token
   silently overrides `--org`, so a token for the wrong org fails as a missing project.

4. Join the device to your WiFi the normal WLED way (its `WLED-AP` captive portal), then open
   WLED at `http://<device-ip>/`. That is the whole of it — there is nothing else to open.

## Trying it

With WLED open in a browser on the same network:

- The **Info** tab shows two rows the usermod adds — `Sentry relay: attached` and
  `Sentry queue: N events`. That is the only place the relay is visible at all, and it is how
  you check it is working without a console.
- `curl http://<device-ip>/sentry/test` captures a message event and relays it within a few
  seconds. The fastest way to confirm DSN, network and relay are all working before involving
  a panic.
- `curl http://<device-ip>/sentry/crash` panics the device 500ms later. It writes a coredump
  and reboots; on the next boot the usermod decodes it, queues the report in NVS, and the
  delivery task ships it as soon as a browser re-attaches.
- Open the browser console for a running commentary: `[sentry] relaying for this device to
  o…ingest.us.sentry.io`, then `[sentry] relayed 987 B -> 200` per event.
- Close every WLED tab, crash it, and reopen WLED later: the report is still there. That is
  the NVS-backed offline buffer, and it is why "only while a browser is open" is a delay
  rather than a hole.

Everything is inspectable with `curl`, which is deliberate:

```
curl http://<device-ip>/sentry/status        # DSN host, release, queue depth, coredump state
curl http://<device-ip>/sentry/relay/poll    # the queued frames, base64
```

## Routes the usermod adds

| Route | |
|---|---|
| `GET /sentry/status` | what the SDK knows: DSN host, release, queue depth, coredump state |
| `GET /sentry/relay/poll` | queued device→host frames, base64 |
| `POST /sentry/relay/status` | the host's verdict on one relayed request |
| `GET /sentry/test` | capture a message event |
| `GET /sentry/crash` | panic on purpose, 500ms from now |

The last two are demo affordances with no UI attached. Everything else is
machine-to-machine.

`GET /sentry/crash` having a destructive side effect is not an oversight — it matches WLED's
own convention, which serves `GET /reset` to reboot the device (`wled00/wled_server.cpp`). It
keeps the trigger to something you can paste into an address bar. If you would rather it were
a POST, that is a one-line change and nothing else depends on the method.

## Design notes

**The HELLO frame is folded into the poll's query string.** On a BLE link the host announces
itself once, on connect; HTTP has no connect, and every poll is already the host saying "I am
here, I speak version N, my frames may be this big". More usefully, it makes reattachment
free — a device that panicked and rebooted has forgotten the host, and the next poll
re-announces it with no reconnect logic on either side. That is not an edge case; it is what
every crash does.

**Delivery runs on its own FreeRTOS task, not in `loop()`.** `RelayTransport::send()` blocks
until the browser answers or the timeout expires — a full round-trip through a UI polling on
a timer. Doing that from `loop()` would stall WLED's render for as long as it takes,
which for a lighting controller is the one cost that would actually disqualify the approach.
The task is priority 1 (below Arduino's loop task) and pinned to core 0.

**Frames are length-prefixed in the poll body.** HTTP hands the other end a byte stream with
no packet boundaries in it; a BLE notification supplies those for free. So the queue stores
`[len u16][frame]` repeated, and the UI cuts them apart again.

**The poll body is base64.** Not aesthetics: this AsyncWebServer's response types are either a
`String` (not binary-safe — an envelope is full of bytes a `String` will not carry) or a
pointer it holds and reads later (also unsafe — the queue it points into is refilled by the
next send). A third more bytes buys a body with neither caveat, and one that prints legibly
under `curl`.

**Idle polling is slow on purpose.** Five seconds when nothing is happening, half a second
while a request is mid-flight. This runs for as long as anybody has WLED open, on a device
whose actual job is rendering an LED strip, and a crash report is worth nothing extra for
arriving four seconds sooner. Polls are chained with `setTimeout` after the previous one
settles rather than fired on a `setInterval`: a poll that outlives its interval — a slow POST
to Sentry sits inside it — would otherwise overlap with the next, both would drain the queue,
and the responses could arrive out of order, landing DATA frames before the BEGIN that sizes
the buffer for them.

**Both ends whitelist the ingest host independently.** The device's `RelayTransport` refuses
any URL whose host is not the DSN's, and the UI refuses anything that is not
`https://<the host /sentry/status reported>` with no embedded username. The UI's check is not
defence against this device — the code came *from* the device — but against a firmware bug, or
a device somebody else put on this network, turning the user's browser into an open proxy
carrying their IP and cookies.

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

## What a crash looks like when it arrives

Verified end to end on an ESP32-PICO-D4 (4MB), phone joined to the device's own `WLED-AP`
with the POST going out over cellular. `abort()` three frames deep in the usermod:

```
IllegalInstruction: in loopTask

  0x4008eaa4  vPortTaskWrapper                             port.c:138
  0x4015733f  loopTask                                     main.cpp:80
  0x401279e1  loop                                         wled_main.cpp:23
  0x4012740b  WLED::loop                                   wled.cpp:103
  0x40123971  UsermodManager::loop                         um_manager.cpp:23
  0x400d2df4  UsermodSentry::loop                          usermod_sentry.cpp:218
  0x400d2846  (anonymous namespace)::sentryDemoCrashOuter      usermod_sentry.cpp:142
  0x400d283e  (anonymous namespace)::sentryDemoCrashMiddle     usermod_sentry.cpp:141
  0x400d2836  (anonymous namespace)::sentryDemoCrashInnermost  usermod_sentry.cpp:140
  0x40094898  abort                                        abort.c:38
  0x4008de8c  esp_system_abort                              esp_system_chip.c:86
  0x4008dec8  panic_abort                                  panic.c:477
```

Functions, files and line numbers, from FreeRTOS's task wrapper through WLED's own `loop()`
into the usermod and down into the panic handler — recovered from a 64K coredump partition on
the *following* boot, then carried over a browser tab. Inlined frames come back too (a
`std::atomic::store` chain, elided here, resolves at the same address as its caller).

The event carries the rest of what you would want: release, `environment`, device model and
architecture, heap total/free, flash size, chip revision, uptime, `esp-idf v5.5.4`, and a
trace id.

### In-app frames

Out of the box every frame is marked not-in-app, so Sentry blames nobody and the issue
culprit stays empty. `scripts/release.sh` prints suggested stack-trace rules at the end of a
run, but they are derived from *your* build directory — and `abs_path` in native debug info is
whatever absolute path the compiler saw, so those rules stop working the moment somebody
builds the same firmware from a different checkout.

These are equivalent and machine-independent. Settings → Projects → *your project* →
Processing → Stack Trace Rules:

```
family:native -app
stack.abs_path:**/wled00/** +app
stack.abs_path:**/usermods/sentry/** +app
```

The blanket `-app` first is deliberate: suspect commits blames the first in-app frame, and a
newlib or ESP-IDF frame counting as yours would blame a file you do not have. (Those arrive
with paths like `/home/runner/work/esp32-arduino-lib-builder/...` — the machine that built the
prebuilt core, not yours.)

## Known issues and limitations

- **Reports flow only while somebody has WLED open.** By design, discussed above. The queue is
  16 NVS slots deep and survives reboots, so nothing is lost in the meantime; it is delayed.
- **The relay does not work while the device is its own access point.** An unconfigured WLED
  opens `WLED-AP`, and in that mode it runs a wildcard captive-portal DNS server
  (`dnsServer.start(53, "*", WiFi.softAPIP())`) that answers *every* query with its own
  address — so the ingest hostname resolves to the ESP32, and the POST goes to a device with
  no TLS stack. The browser reports a bare network error with nothing pointing at the cause.
  Join the device to a real network; that is the setup this is for. (A client that already
  cached the hijacked answer keeps using it, too — observed on iOS well past the 60-second TTL
  it was served with, cleared only by toggling Wi-Fi.)
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

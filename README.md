<p align="center">
  <a href="https://sentry.io/?utm_source=github&utm_medium=logo" target="_blank">
    <img src="https://sentry-brand.storage.googleapis.com/sentry-wordmark-dark-280x84.png" alt="Sentry" width="280" height="84">
  </a>
</p>

# sentry-micro

**Sentry SDK for microcontrollers.** Captures crashes, reset reasons and device telemetry on
an ESP32 and delivers them to Sentry over *any* connectivity — WiFi directly, or by handing a
fully-formed Sentry envelope to a companion app that just relays the bytes.

> **Status: early prototype.** This is a Sentry hackweek project and is **not officially
> supported by Sentry** — it is not a released SDK. It does the whole job end to end: an
> ESP32 panics, reboots, and the crash arrives in Sentry as a **symbolicated backtrace with
> function names, file names, line numbers and source**, over WiFi or relayed through a
> host. We accept pull requests if you are willing to fix bugs and add features; if there is
> enough interest we may invest more into this. See [Roadmap](#roadmap) for what is and is
> not done, and [`ESP32_SENTRY_HACKWEEK.md`](ESP32_SENTRY_HACKWEEK.md) for the proposal.

> **⚠️ Not production-hardened.** The examples demonstrate the pipeline; they are not built
> to be copied into a shipped device, and two trust decisions are left to you before you
> rely on this against real hardware:
>
> - **TLS certificate verification is off by default** in the WiFi transport. Your traffic
>   is encrypted but *unauthenticated* — a device will talk to any server that answers its
>   DSN host. Call `set_ca_cert()` with a real root before trusting it; the example ships
>   one in [`certs.h`](examples/wifi_basic/src/certs.h). See the [WiFi transport](#wifi-transport--https-post-straight-to-ingest) section.
> - **The relay is an open proxy by design.** A companion app performs HTTPS on the device's
>   behalf, and the DSN-host whitelist lives on the device *and* must be enforced
>   independently on the relay side. That is a convention, not something the protocol
>   enforces. See the [Serial relay](#serial-relay-transport--when-the-device-has-no-network) section.

This is a concrete answer to
[sentry-native#915](https://github.com/getsentry/sentry-native/issues/915), where Sentry
declined to port `sentry-native` to microcontrollers — it assumes virtual memory, a
filesystem and threads — and floated that *"there might be a sentry-micro SDK at some
point"*. The stated blocker was that community signal needed to be stronger. This is a
working demonstration that the ingest and symbolication side already supports it.

```
StoreProhibited
accessing 0x00000000

  esp32dev.elf  0x4016984d  demo_crash_innermost (main.cpp:221)
  esp32dev.elf  0x400d2e89  demo_crash_middle    (main.cpp:224)
  esp32dev.elf  0x400d2e92  demo_crash_outer     (main.cpp:226)
  esp32dev.elf  0x400d3637  setup                (main.cpp:412)
  esp32dev.elf  0x400da349  loopTask             (main.cpp:42)
```

## Why this is not `sentry-native`

[`sentry-native`](https://github.com/getsentry/sentry-native) targets "protected-mode"
operating systems — virtual memory, a filesystem, threads. A microcontroller has none of
those, which is why Sentry's own position on
[sentry-native#915](https://github.com/getsentry/sentry-native/issues/915) is that MCU support
belongs in a *separate* SDK rather than a port. This is a prototype of that SDK.

What that means concretely:

- **No allocation on the reporting path.** A crash handler cannot trust the heap — it may be
  exhausted, fragmented, or the very thing that failed. All SDK state is statically allocated
  and fixed-size, so its RAM cost shows up in the map file.
- **No filesystem assumed.** Offline buffering works against NVS directly, or against a
  filesystem you already mount.
- **The device builds the envelope; the transport is a dumb pipe.** Sentry's ingest protocol is
  SDK-agnostic — a plain `POST /api/<project>/envelope/` with an `X-Sentry-Auth` header. The
  library owns every bit of that; delivery is one virtual `send()` call behind
  [`sentry_transport_t`](src/core/sentry_transport.h). That is what lets the same core
  reach Sentry over WiFi, BLE, serial or LoRa, and why a companion app needs *zero* Sentry
  knowledge to relay for a device with no internet of its own.

We interoperate with Sentry's **ingest and symbolication**, not with `sentry-native`'s code.

## Repository layout

```
src/                        the library
  sentry_micro.h            public API — the only header you include (C, plus the C++ layer)
  sentry_micro.c            init/close and SDK state
  sentry_micro.hpp          inline C++ wrapper, pulled in automatically by the header
  sentry_micro_cxx.cpp      the one non-inline bit of it (the Arduino Serial logger)
  core/                     portable freestanding C: no Arduino, no ESP-IDF, host-testable
    sentry_boot.h           version, size limits
    sentry_dsn.{h,c}        DSN parsing, ingest URL, auth header, host whitelisting
    sentry_json.{h,c}       fixed-buffer JSON writer
    sentry_envelope.{h,c}   event + envelope construction, debug ids
    sentry_base64.{h,c}     encoding for the relay protocols
    sentry_buffer.{h,c}     offline ring-buffer policy over a storage vtable
    sentry_transport.{h,c}  the delivery interface
  device/                   chip-specific: collection and storage
    sentry_device_esp32.c   chip info, reset reason, entropy, clock
    sentry_storage_nvs.c    buffer storage in NVS
    sentry_storage_fs.cpp   buffer storage on LittleFS/SPIFFS/SD
  transport/
    sentry_transport.hpp        C++ base class over the C interface
    sentry_transport_wifi.*     HTTPS straight to ingest
    sentry_transport_serial.*   relay through a USB host
    sentry_transport_auto.hpp   picks a route per delivery attempt, from a list of others
test/                       host unit tests (Unity), run with `pio test`
examples/
  wifi_basic/               a real sketch you can flash — WiFi + SDK init
scripts/                    release.sh (build + stamp + upload), serial_relay.py
.github/actions/            the same chain as a reusable GitHub Action
partitions/                 reference partition tables (with a `coredump` partition)
platformio.ini              host test project (firmware builds live in the examples)
```

Two splits are load-bearing rather than tidy:

**`core/` vs `device/`.** Everything in `core/` compiles on a laptop, so DSN parsing — and
later envelope construction — is tested in milliseconds by `pio test` instead of by flashing
a board. `device/` is the porting boundary: when ESP8266 or nRF52 arrives, it gets a sibling
of `sentry_device_esp32.c` and `core/` does not move.

**C is the API; C++ is a wrapper.** The whole SDK, including the transport interface, is
plain C, so it is usable from an ESP-IDF component with no C++ runtime. The C++ names are
inline forwarders over the same state — `sentry::init()` *is* `sentry_init()`, and a
`sentry::Transport` subclass hands the core the same `sentry_transport_t` vtable a C author
would fill in by hand. There is one representation, not two that can drift.

## Quickstart

Requires [PlatformIO](https://platformio.org/install/cli).

```bash
git clone https://github.com/getsentry/sentry-micro.git
cd sentry-micro/examples/wifi_basic

cp src/secrets.example.h src/secrets.h   # then edit: WiFi SSID/password + your DSN
pio run -e esp32dev -t upload -t monitor
```

You should see the SDK report the endpoint it will POST to and what it knows about the boot:

```
── sentry-micro ──────────────────────────────
sdk           : sentry.micro.esp32 0.1.0
enabled       : yes
release       : sentry-micro-example@0.1.0
ingest host   : o1234.ingest.us.sentry.io (tls: yes)
envelope url  : https://o1234.ingest.us.sentry.io/api/4507/envelope/
── device ────────────────────────────────────
chip          : ESP32-S3 rev 2, 2 core(s)
device id     : a1b2c3d4e5f6
reset reason  : panic  <- previous boot crashed
```

Using it in your own firmware — add to your `platformio.ini`:

```ini
lib_deps = https://github.com/getsentry/sentry-micro.git
```

Arduino / C++:

```cpp
#include <sentry_micro.h>

void setup() {
    sentry::Options options;
    options.dsn = "https://<key>@<org>.ingest.sentry.io/<project>";
    options.release = "my-firmware@1.0.0";   // groups events; symbolication uses debug_id
    sentry::init(options);                   // call this first — it reads the *previous* boot
}
```

ESP-IDF / C — the same SDK, no C++ runtime required:

```c
#include <sentry_micro.h>

sentry_options_t options;
sentry_options_defaults(&options);           // C has no default member initialisers
options.dsn = "https://<key>@<org>.ingest.sentry.io/<project>";
options.release = "my-firmware@1.0.0";
sentry_init(&options);
```

`init` returns `false` on a bad DSN and leaves the SDK disabled rather than half-configured.
Crash reporting must never be load-bearing: a firmware should always be able to ignore that
return value and carry on.

## Reporting something that is not a crash

Crashes report themselves. For everything else — an OTA that failed its signature check, a
sensor that stopped answering, a config that would not parse — there is one call:

```cpp
sentry::capture_message(SENTRY_LEVEL_WARNING, "OTA aborted: bad signature");
```

```c
sentry_capture_message(SENTRY_LEVEL_WARNING, "OTA aborted: bad signature");
```

It builds the event, frames the envelope, sends it, and buffers it for a later retry if
there is no route yet. The message is used during the call and never retained, so a
`snprintf` into a stack buffer is fine. It costs `SENTRY_MICRO_ENVELOPE_BUFFER_BYTES` of
the caller's stack and no heap at all, and it sends inline — fine at boot, worth reading
[Delivery model](#delivery-model) before calling it from a render loop.

### It is throttled, on purpose

The API is easy to call from a loop, and a loop runs forever. A sensor that starts failing
at 50 Hz does not produce one issue in Sentry — it produces 50 events a second, from a
device nobody is watching, against a quota you are paying for. And the first thing you lose
when a quota runs out is crash reports.

So two limits apply before an event is built, both configurable and both able to be turned
off with `0`:

| Option | Default | What it does |
| --- | --- | --- |
| `message_repeat_window_ms` | `10000` | The same message at the same level is sent at most once per window |
| `max_messages_per_minute` | `10` | Ceiling on everything else, whatever it says |

Repeats are checked first, so a message stuck in a loop cannot spend the budget a
*different* message needed. The window runs from the moment the previous identical message
**finished being sent**, and a suppressed call never moves it — so a caller faster than the
window still reports once per window rather than going silent after the first event.

Timing it from completion rather than from the start of the call is what makes the rule
work at all. `capture_message()` sends inline, and on a device with no route one send
blocks for the transport's timeout — 15.2 s measured on hardware, against `SerialTransport`'s
15 s default. Measured from the start of the call, a window shorter than that has always
already elapsed by the time the caller loops round, so nothing is ever suppressed. That was
found on a board, not in review: a twenty-iteration loop produced seventeen events and
evicted thirteen envelopes from the offline buffer.

`sentry_capture_message()` returns `SENTRY_SEND_RATE_LIMITED` when the throttle drops
something, and `sentry_suppressed_count()` says how many it has dropped since init. Crash
reports do not go through any of this: a throttle that could eat the panic you rebooted
from would be worse than no throttle.

## WiFi transport

For a device on WiFi, this is all of it:

```cpp
#include <sentry_micro.h>
#include <transport/sentry_transport_wifi.hpp>

static sentry::WiFiTransport transport;   // file scope — the SDK stores a pointer

void setup() {
    sentry::Options options;
    options.dsn = "https://<key>@<org>.ingest.sentry.io/<project>";
    sentry::init(options);
    sentry::set_transport(transport);
}
```

It refuses to POST to any host but the one in your DSN, with no configuration — matching is
exact, so `evil-sentry.io`, `sentry.io.evil.com` and `https://sentry.io@evil.com/` are all
rejected, and redirects are disabled so a `302` cannot move the auth header somewhere else.

**TLS verification is off by default.** The connection is encrypted but *unauthenticated*
until you supply a root:

```cpp
transport.set_ca_cert(SENTRY_INGEST_CA_CERT);
```

That is not a default because pinning a root that later expires bricks reporting on every
deployed device simultaneously, and a maker with no CI has no way to push a new one. The
trade belongs to whoever ships the firmware, so the SDK makes it explicit and logs a warning
the first time it sends without one. The example turns this on out of the box — it ships the
DigiCert Global Root G2 cert in [`certs.h`](examples/wifi_basic/src/certs.h), which verifies
the public Sentry cloud — and swaps in a different root only if you self-host.

**Do not call a TLS transport from a panic handler.** mbedTLS allocates several KB during a
handshake and the heap is exactly what you cannot trust right after a crash. The design does
not need it to: a crash is detected on the *next* boot from the reset reason and the coredump
partition, then reported from normal runtime. That is why the no-allocation rule is stated
for the core and not for transports.

Footprint on `esp32dev` with the WiFi transport, TLS and the full example:
**923,937 bytes flash** (50% of a 1.75 MB OTA slot) and **47,836 bytes RAM** (15% of 320 KB).
mbedTLS dominates both.

### Building without TLS

`WiFiClientSecure` is what pulls mbedTLS into the image, and a runtime `if (is_https)`
keeps it linked whether or not any device ever takes that branch. `SENTRY_MICRO_WIFI_TLS=0`
removes the branch at compile time so the linker can drop it:

```ini
build_flags = -D SENTRY_MICRO_WIFI_TLS=0
```

Measured on `esp32dev`, clean builds of `wifi_basic` either way:

| | TLS on | TLS off | Saved |
| --- | --- | --- | --- |
| Flash image | 941,248 B | 813,840 B | **127,408 B (124 KB)** |
| `.text` | 777,445 B | 685,601 B | 91,844 B |
| RAM (`.bss`) | 22,497 B | 21,593 B | 904 B |

That is worth having on a 4 MB module with an OTA layout, where two app slots plus a
filesystem leave less headroom than the flash size suggests.

The build then talks **plain HTTP only**. An `https://` ingest URL is refused with
`SEND_REJECTED` rather than downgraded — sending the envelope in clear would put the DSN's
write key on the wire — so this is only useful against a self-hosted endpoint on HTTP, or a
relay. Sentry's cloud is HTTPS-only. `set_ca_cert()` is compiled out too, so a build that
opts out of TLS and still tries to pin a certificate fails to compile rather than silently
ignoring it.

## Serial relay transport — when the device has no network

A device that cannot reach the internet is still plugged into a machine that can. This hands
the whole request over the USB cable and lets a small script perform it:

```cpp
static sentry::SerialTransport transport;
sentry::set_transport(transport);
```

```bash
export SENTRY_MICRO_DSN='https://...'
scripts/serial_relay.py --port /dev/cu.usbserial-XXXX     # replaces `pio device monitor`
```

The script passes device logs through to your terminal and answers relay requests:

```
[sentry] transport: serial relay
[relay] 781 bytes -> https://o…ingest.us.sentry.io/api/…/envelope/
[relay] HTTP 200 in 470ms
[sentry] delivered (http 200)
```

This is worth more than bench convenience. **It is the same architecture as the BLE relay** —
the device builds a complete Sentry request and something else moves the bytes — validated
over a link that is trivial to debug. The script knows nothing about Sentry beyond "POST
these bytes to this URL", which is exactly the property that will let a companion app support
the SDK in ~20 lines. The device-side code is identical either way.

Every field is base64 because a URL and an auth header contain spaces and an envelope is
ndjson that *contains newlines*; encoding removes every framing question. The device encodes
in fixed 48-byte chunks straight to the port, so a 4 KB envelope costs 65 bytes of stack
rather than the 5.4 KB a buffered encode would need out of the loop task's 8 KB.

**The whitelist is the point, not a nicety.** The device is asking another machine to POST
arbitrary bytes to an arbitrary URL. The relay refuses anything but the DSN's host over
https — exact match — so a buggy or hostile device cannot use its host as an open proxy.
That is non-negotiable when the relay is a user's *phone*, and it is enforced identically
here. Verified refused: `evil.com`, `…sentry.io.evil.com`, `evil-…sentry.io`, a plaintext
downgrade, `https://…sentry.io@evil.com/`, `file:///etc/passwd`, and `169.254.169.254`.

## Picking a route automatically

A device with more than one way to reach Sentry — WiFi normally, a serial or BLE relay as a
fallback — needs something to choose between them on every delivery attempt, not just once
at boot. `AutoTransport` is that: an ordered list of other transports, re-evaluated every
time.

```cpp
#include <transport/sentry_transport_auto.hpp>

static sentry::WiFiTransport wifi_transport;
static sentry::SerialTransport serial_transport;
static sentry::AutoTransport transport({&wifi_transport, &serial_transport});

sentry::set_transport(transport);
```

It calls each transport's `is_available()` in order and delegates to the first one that says
yes — cheap and non-blocking, so a dead transport costs nothing beyond that check, never a
connect timeout. Because that check runs again on every attempt rather than once, a device
that boots with no WiFi and starts on the relay picks up WiFi transparently the moment it
associates, with no reboot and no code watching for the transition.

**Ordering matters, and it is easy to get backwards.** `SerialTransport::is_available()`
always returns `true` — there is no way to detect a listener on a bare UART, so it only
discovers the truth via its own timeout inside `send()`. Anything placed *after* it in the
list is therefore unreachable, since `AutoTransport` always picks the first available one.
Put transports that can tell the truth about availability first, and anything that always
claims to be available last: `{&wifi, &relay, &serial}`, never `{&wifi, &serial, &relay}`.

## Offline buffering

An envelope that cannot be delivered is persisted and retried, rather than dropped. This
matters more here than on a desktop: the most valuable event this SDK produces — the report
of the crash that just happened — is built at boot, *before* the radio has associated.

```cpp
sentry_enable_buffering(sentry_storage_nvs(16));   // or storage_fs(), below
...
void loop() {
    if (sentry_buffered_count() > 0) sentry_flush(2);   // on an interval, not every pass
}
```

**Storage is pluggable**, because the right answer depends on what the firmware already has:

| Backend | Use when |
| --- | --- |
| `sentry_storage_nvs(slots)` | no filesystem; uses the stock 20 KB `nvs` partition |
| `sentry::storage_fs(LittleFS, slots)` | you already mount LittleFS / SPIFFS / SD |

`storage_fs` takes `fs::FS`, the Arduino base class, so pass whatever object you already
mounted. **Neither backend mounts, formats, or erases anything.** NVS never erases to reclaim
space, and the filesystem backend never calls `begin()` — a reporter that reformatted a
partition of user data to report a crash would be worse than the crash. Everything is
confined to a `sentry` namespace / `/sentry` directory.

**Sizing `slots`** is a flash budget question, not a correctness one: `sentry_storage_nvs()`
accepts any count from 1 up to `SENTRY_NVS_MAX_SLOTS` (64) and rejects anything outside that
range rather than clamping it. An envelope runs roughly 1 KB, so slots × 1 KB is what you are
spending against whatever partition you gave the buffer — the stock 20 KB `nvs` partition
makes 16 a comfortable ceiling before NVS has no room left for anything else you keep there.
Weigh that against how long the device is realistically offline at a stretch: more slots
survive a longer outage, at the cost of the flash they occupy whether or not they are ever
used. This is one ring shared by every envelope type with no priority between them, so a
size chosen too small is what an unrelated high-volume category — Application Metrics
today, see below — can evict a crash report from.

Writing your own is five functions (`write`, `read`, `erase`, `load_meta`, `save_meta`) —
the same vtable pattern as transports, which is what lets the ring logic be host-tested
against a plain array.

## Symbolication: making backtraces readable

Sentry never sees your code. It matches an event to an uploaded ELF by `debug_id`, derived
from the firmware's GNU build-id — **not** by release. `sentry-cli debug-files upload` takes
no release argument, so a release can be renamed, or left unset entirely, without breaking a
single stack trace. (JavaScript source maps *are* release-scoped, which is where the
assumption usually comes from.) The release matters for grouping, release health and suspect
commits; symbolication is independent of it.

One command does the whole chain:

```bash
scripts/release.sh -e esp32dev -r 'my-firmware@1.2.3'
```

which picks a build-id, compiles it in, stamps it into the ELF, and uploads the ELF to
Sentry with `sentry-cli`. After that, addresses in an event resolve to functions and lines.

The only credential it needs is a token — org and project are read out of the DSN, since
both are already in it and `sentry-cli` accepts numeric ids:

```bash
brew install getsentry/tools/sentry-cli          # once
export SENTRY_AUTH_TOKEN='sntrys_...'            # Sentry -> Settings -> Auth Tokens
export SENTRY_MICRO_DSN='https://...'
scripts/release.sh -e esp32dev
```

`release.sh` also tells Sentry **which commits went into the build**, so an issue answers
"what changed" and not just "something broke" — usually the more useful half on a device you
cannot attach a debugger to. Add the repository to your Sentry organization's integrations
and you additionally get suspect commits and links back to GitHub; without one, the commit
list still lands, under a repository named after the git remote. `--no-commits` opts out.

This needs real git history, and `actions/checkout` defaults to `fetch-depth: 1` — so a
shallow clone is the *normal* state of a CI workspace, not an unusual one. Left alone it
associates exactly one commit, which is not "fewer commits": it is a release claiming the
build contains a single change when it contains a hundred, and it renders identically to
the truth.

So the Action deepens the checkout itself (`git fetch --unshallow`, using the credentials
`actions/checkout` already left behind), and if that is not possible `release.sh` refuses
the release rather than attaching a commit range it knows is wrong. `fetch-depth: 0` avoids
the round trip; `--no-commits` / `set-commits: false` ships without them deliberately.

An **organization auth token** (scope `org:ci`) is the right kind and is what CI should use;
it embeds its own org, which is why the script does not pass one. `sentry-cli login` works
too for a browser flow. Pass `--no-upload` to build and stamp without talking to Sentry.

Verified against a real project — `sentry-cli debug-files check` reports the uploaded ELF as
`Usable: yes` with `symtab, debug, unwind`, under the same `debug_id` the device puts in
`debug_meta`. One caveat worth knowing before the frames land: it also reports
`Arch: unknown` for Xtensa, which is not a recognised architecture in Sentry's symbolic
library. Whether that affects resolution can only be answered once real frames exist — the
RISC-V targets (C3/C6) are unlikely to have the same question.

**Do not use `-Wl,--build-id`.** On ESP32 the linker marks the note `ALLOC` and places it at
the start of IRAM, pushing `.iram0.vectors` off `0x40080000`:

| | `.note.gnu.build-id` | `.iram0.vectors` |
| --- | --- | --- |
| without the flag | — | `0x40080000` ✅ |
| with the flag | `0x40080000` | `0x40080024` ❌ |

`VECBASE` requires 1 KB alignment, so the first interrupt jumps into the note. Measured, not
theorised — the board boot-loops with `rst:0x10 (RTCWDT_RTC_RESET)`. `scripts/stamp_build_id.py`
adds the note *after* linking as a **non-ALLOC** section instead: present in the ELF where
`sentry-cli` reads it, absent from flash. That also solves the chicken-and-egg — the firmware
must know its own build-id to report it, and a linker-computed one only exists after linking,
so we choose the value and use it in both places.

`image_addr` and `image_size` matter as much as the build-id, and fail more quietly. Sentry
resolves an address by computing `instruction_addr - image_addr` and looking the result up
against symbols normalised by the object's own load address — so `image_addr` must equal the
ELF's lowest `PT_LOAD` address (`0x3f400020` on ESP32, *not* 0), and `image_size` is what
decides which module a frame belongs to. Get either wrong and every frame renders as
`<unknown>`: the event arrives, the addresses look right, and nothing says why.

That is why `release.sh` builds in a loop rather than once. The values only exist after the
link, so the first pass measures and the next bakes them in — but baking them in *moves*
them. On Xtensa a small constant is a two-byte `movi.n` and a large one becomes a four-byte
literal-pool entry, so compiling in a real size grew this image by `0x1c` bytes, and the
size in the firmware then described the previous build. It re-measures and re-bakes until a
pass produces an ELF matching the numbers it was built with — usually three passes — and
refuses to ship if it never settles.

### Suspect commits: two settings, derived for you

Symbolication works out of the box. Getting Sentry to point at the *likely* commit needs two
project settings that it cannot infer for a native project — automatic code mappings cover
JavaScript, Python, Java, PHP, Ruby, Go, C# and Kotlin, and nothing else.

`release.sh` prints both at the end of every upload, read out of the ELF it just built:

```
Code mapping   (Settings -> Integrations -> your repo -> Code Mappings)
  Stack trace root : /home/runner/work/chromabay/chromabay/
  Source code root : (leave empty — the repository root)

Stack trace rules   (Settings -> Projects -> your project -> Processing)
  family:native -app
  stack.abs_path:/home/runner/work/chromabay/chromabay/** +app
  stack.abs_path:**/.pio/libdeps/** -app
```

**The code mapping** exists because the compiler records the build machine's absolute paths,
and nothing lines up with the repository until that prefix is stripped. Only the build knows
it, which is why this is printed from the ELF rather than guessed — and why the run that
matters is the one in CI.

**The stack trace rules** exist because suspect commits blames the first *in-app* frame. The
example firmware is built from 21 distinct directories and exactly one is ours; the rest are
Espressif's CI, a GitLab runner and two strangers' home directories, baked into prebuilt
libraries. Enumerating those is hopeless, so the rules invert it: nothing is in-app until
proven otherwise, then your repository is added back. Without them a `newlib` frame counts
as your code and Sentry blames a file you do not have.

Run it against any ELF directly:

```bash
scripts/sentry_config.py firmware.elf --project-dir examples/wifi_basic
```

### Checking it actually works

Every step of this chain reports success whether or not it worked, and the failures land far
from the mistake — debug files that match nothing, a code mapping one directory off, a
"suspect commit" section that is simply absent. `doctor.py` asks the questions out loud,
before something crashes:

```bash
export SENTRY_AUTH_TOKEN='...'          # needs project:read and org:read
scripts/doctor.py --elf firmware.elf --org my-org --project my-project \
    --release 'my-firmware@1.2.3'
```

```
[  ok  ] debug files uploaded for 0760011f-9f6c-a142-57f0-a8f0dcc68e02
[  ok  ] release my-firmware@1.2.3 exists
[  ok  ] release is finalized
[  ok  ] release carries 14 commit(s)
[ FAIL ] no code mappings
         Sentry does not create these automatically for native projects.
         Run scripts/sentry_config.py for the values.
```

It checks the code mapping against the ELF's *actual* build paths rather than for mere
existence, since a mapping that is one directory off looks configured and matches nothing.
Read-only, and it distinguishes "this setting is missing" from "this token cannot see it" —
the upload-only token a release uses cannot read project settings, and reporting that as a
missing setting would send you to fix something that is already right.

Each build variant gets its own id, derived from `release + env`. That is required, not
cosmetic: every board in a matrix is a distinct binary, and resolving addresses against the
wrong one produces confidently wrong function names.

### In CI: one step instead of a copied script

`release.sh` does the whole chain, but telling an adopter to copy a bash file is not an
integration. The same chain is packaged as a composite action:

```yaml
- uses: getsentry/sentry-micro/.github/actions/upload-debug-files@main
  with:
    project-dir: firmware
  env:
    SENTRY_AUTH_TOKEN: ${{ secrets.SENTRY_AUTH_TOKEN }}
    SENTRY_MICRO_DSN: ${{ secrets.SENTRY_MICRO_DSN }}
```

That builds **every** `[env:...]` in `firmware/platformio.ini`, stamps each one, uploads its
debug files, and fails the job if any step did not do what it claimed.

The default matters more than the convenience. Symbolication is per-binary, so every board
and feature variant needs its own upload — WLED ships dozens — and a variant nobody uploaded
is invisible: its firmware works, its events arrive, and only its users get raw hex. So the
list of variants is read out of `platformio.ini` rather than written into the workflow.
Adding a board to that file adds it to the release; leaving one out fails the build until
somebody says, in `skip-environments`, that its users are meant to go without.

For a matrix job per variant, generate the matrix from the same source:

```yaml
jobs:
  plan:
    runs-on: ubuntu-latest
    outputs:
      environments: ${{ steps.list.outputs.environments }}
    steps:
      - uses: actions/checkout@v5
      - uses: getsentry/sentry-micro/.github/actions/list-environments@main
        id: list
        with: { project-dir: firmware }

  upload:
    needs: plan
    strategy:
      matrix:
        environment: ${{ fromJSON(needs.plan.outputs.environments) }}
    ...
```

`.github/workflows/release.yml` in this repository is that workflow, running against the
`wifi_basic` example — copy it and change `project-dir`.

Three checks in there exist because the corresponding mistake is otherwise silent:

| Check | What it catches |
| --- | --- |
| `sentry-cli debug-files check` on the stamped ELF | `objcopy` exits 0 whether or not the note landed. An unstamped ELF uploads happily and resolves nothing. |
| `--id <debug_id> --require-all` on upload | "matched 0 files" is a successful exit code otherwise. |
| `--wait` | Without it the upload returns when the bytes are accepted, not when the server accepts the *file*. |

Plus one the scripts do themselves: two variants that somehow derive the same `debug_id`
fail the release, because Sentry would resolve one binary's addresses against the other and
print function names that look entirely plausible.

## Linking a device event to what caused it

A crash on the device and a session replay in the app that provoked it are the same
incident. Sentry joins them on a shared `trace_id`, so the device's job is to carry an id it
was handed, attach it to what it emits, and then forget it.

```cpp
sentry::trace_adopt(sentry_trace_header, baggage_header);   // request arrives
handle_the_request();                                       // any event here joins the trace
sentry::trace_release();                                    // request done
```

The SDK does not care how those two strings reached the device — a BLE characteristic, an
HTTP header, a field in your own protocol. By the time they get here they are two strings.

**A trace is a unit of work, not a lifetime.** One trace per boot is the tempting design and
it is wrong: it stays open for days, which the trace UI and the sampling model both assume
never happens. The bounded things that *are* traces are an app-initiated operation, a boot,
an OTA — and `sentry::trace_start()` begins one the device originates.

That makes the device behave like a backend, which is also why the release step matters. A
device that keeps the last trace it saw will attach a panic three hours later to an
interaction that had nothing to do with it. That link renders exactly like a real one.

**`replay_id` comes along for free.** It rides in the `baggage` header whenever the calling
app has a replay running, and lands in the event's `replay` context — so the Sentry issue
links straight to the recording of the person who caused it. It is scoped to the request
like everything else here.

**A trace from a different organization is refused.** `baggage` may also carry a
`sentry-org_id`, and `trace_adopt()` compares it against this device's own
(`Options::org_id`) before joining — a known mismatch would mix telemetry across accounts,
so the device becomes the head of a fresh trace instead of adopting one that isn't its own.
Neither side has to know its org id for the request to proceed normally; set
`Options::strict_trace_continuation` if an *unknown* id on either side should be treated as
suspicious too.

**Adopting does not disturb a recovered crash.** The app that comes to collect a crash
report usually connects and offers a new trace in the same breath, so the trace the device
*died* inside is kept apart from the one it is serving *now* — reporting the last boot
before or after `trace_adopt()` gives the same answer. Found by the first integration
rather than in review, and fixed in the SDK instead of written down as an ordering rule.

If one trace covers a whole connection rather than one command, note that a Sentry replay
ends after 60 minutes, or 15 minutes without a click or navigation. A `replay_id` held past
that still links, but it names the session that *was* recording rather than the interaction
at hand.

### Surviving the crash

A crash is only reported on the *next* boot, so the active trace has to outlive the panic.
On ESP32 it is kept in RTC slow memory (`RTC_NOINIT_ATTR`), which is cleared on power-on but
survives a software reset and a panic — precisely the lifetime wanted. A cold boot forgets
it, because there was no operation in flight to remember, and the crash then carries no
trace. That is the correct answer rather than a gap.

### Spans: what the device did, not only what went wrong

Trace context alone makes the device visible in a trace *when it fails*. A transaction makes
it a participant:

```cpp
sentry::Transaction txn;                       // yours, on the stack — 688 bytes
sentry::transaction_start(txn, "set-colour", "device.operation");

auto *decode = sentry::start_child(txn, "ble.decode");
sentry::span_set_attribute(decode, "free_heap", ESP.getFreeHeap());
sentry::span_finish(decode);

sentry::transaction_finish(txn);
```

The verbs match sentry-native — `_start` / `_finish`, child spans started from their parent
— so the shape is familiar from the desktop C SDK. What deliberately differs is ownership:
sentry-native returns heap-managed handles, and this SDK does not allocate. **You declare the
transaction where the operation runs**, the same way a crash report is read into a
`sentry_coredump_t` you own. A device that never traces carries none of it.

`start_child()` returns `nullptr` when the transaction is full, and passing `nullptr` onward
is a no-op, so firmware never has to check. Dropped spans are counted and tagged
`spans_dropped:true` — a trace quietly missing spans reads as a complete picture of a
simpler operation than the one that ran.

`span_set_attribute()` enriches the trace with numbers — and is **not** the same thing as
Sentry's Application Metrics. Those are a separate product (`count` / `gauge` /
`distribution`, their own envelope item, their own explorer), independent of trace sampling,
and **this SDK does not implement them yet**. Sentry's guidance: span attributes for
enriching existing traces, Application Metrics for anything that must not be sampled away.

That gap is sharper on a device than on a server. The numbers a microcontroller most wants
to report — heap trending down over a week, RSSI, frame time — belong to no operation, and a
span attribute needs one to hang off. So the SDK emits Application Metrics too:

```cpp
sentry::metric_count("ble.disconnect");                       // counter
sentry::metric_gauge("device.free_heap", ESP.getFreeHeap(), "byte");
sentry::metric_gauge("wifi.rssi", WiFi.RSSI());
```

**Recording does not send**, and that is the whole point on this hardware.
`transaction_finish()` posts inline and blocks the loop task, so a path that runs several
times a second cannot be traced at any sampling rate — but it can be counted. These add to a
fixed table and return; the table rides the next `sentry_flush()`, on whatever interval your
`loop()` already uses.

They do need a clock, though — every metric carries a timestamp. Until the device has been
told the date, the table keeps accumulating and nothing is sent: a counter covering a longer
interval is still true, which is why metrics *wait* where a transaction's stale duration
would make it drop.

The table holds `SENTRY_MICRO_MAX_METRICS` (8) distinct **names** — a counter hit a thousand
times a second is still one slot. A ninth name is dropped and counted rather than evicting
one that is already accumulating, because a running total that silently restarts is worse
than one that never started: only the second is visible. `sentry_metrics_dropped_count()`
reports it.

Integers only, because printf's float support is an opt-in linker flag on this target that
firmware routinely leaves off.

**The table costs 272 bytes of permanent RAM whether or not you ever call these** — unlike a
transaction's spans, a metric has to survive across flushes rather than living on a caller's
stack for one operation, so it is a permanent `g_state` field the same way the log ring
below is. `SENTRY_MICRO_METRICS_ENABLED=0` removes it, along with `sentry_metric_count()` /
`sentry_metric_gauge()` / `sentry_metrics_dropped_count()` — see
[Logs](#logs-a-continuous-console-correlated-by-trace) for the measured table; the two
toggles are independent and combine.

### What it costs

| | |
| --- | --- |
| Flash, when used | **3.4 KB** (0 if you never call it — the linker drops it) |
| Permanent RAM | **0** |
| `sentry_transaction_t` on your stack | **688 B** at the default 4 spans |
| Peak stack in `transaction_finish()` | ~2.7 KB, including the 2 KB envelope buffer |

Four spans covers "decode, validate, apply, ack". Raising `SENTRY_MICRO_MAX_SPANS` costs
128 bytes of stack each and has to stay inside `SENTRY_MICRO_ENVELOPE_BUFFER_BYTES` at
roughly 150 bytes of JSON per span — Arduino's loop task has 8 KB of stack in total, and a
TLS handshake already wants several KB of it.

**Durations come from the monotonic clock, so they are always right.** Only the position on
the timeline needs a real date — and if the device has never been told one,
`transaction_end()` sends nothing and says so. A duration has no server-side substitute: the
server observes one moment, and a duration needs two. Errors are unaffected; they stay
reportable with no clock at all.

**Setting the clock is the application's job.** It needs a transport, a message format and a
drift policy, all of which belong to whoever built the device. ChromaBay seeds it from its
companion app over BLE and from NTP when WiFi is up; the SDK only reads it.

Scoped to app-initiated operations. A boot transaction would start before anything has told
the device the time, which on a BLE-only device may never happen at all on a given power
cycle. `sentry-sample_rand` is parsed from `baggage` and carried for later use, but the
device honours the caller's sampling decision rather than making its own.

### Logs: a continuous console, correlated by trace

A deployed device's console is the one thing you most want and cannot have — it is a cable
you are not attached to. `sentry_log()` mirrors it:

```cpp
sentry::log(SENTRY_LEVEL_WARNING, "WiFi reconnect attempt %u", attempt);
```

**Recording does not send, the same as a metric** — it writes into a fixed ring and rides
the next `sentry_flush()`. Unlike a metric, each line remembers whatever trace was active
when it was *recorded*, not whatever happens to be active when the ring is flushed later —
the same way a breadcrumb attaches to what the device was actually doing, rather than to
nothing (or something unrelated) by the time the batch goes out. A line recorded while idle
is still held and sent, just without that attachment: logging the console is the point even
when nothing else is going on.

The message is formatted printf-style into a fixed `SENTRY_MICRO_LOG_BODY_LEN`-byte buffer
(81 bytes by default, a conventional terminal line width) and truncated to fit rather than
dropped — a shortened line you can still read beats losing it entirely. Truncation is
computed from `vsnprintf()`'s own return value, not predicted at compile time, and reported
two ways: `sentry_logs_truncated_count()` since init, and a per-line `t7d` attribute
(present only when true) once the line reaches Sentry.

The ring holds `SENTRY_MICRO_MAX_LOGS` lines (6 by default) and evicts the oldest once
full — unlike the metrics table, there is no running total to protect here, so the newest
line displacing the old one is the right trade for a continuous stream.
`sentry_logs_dropped_count()` reports how many were evicted before they were ever sent.

### What it costs

| | |
| --- | --- |
| Permanent RAM | **832 B** at the defaults (`sentry_log_ring_t`: 6 × 136-byte entries) |
| Flash, always linked | ~1.5 KB — `flush_logs()` runs on every `sentry_flush()`, whether or not the firmware ever calls `sentry_log()` |

Unlike a transaction, this is not opt-in by usage: the ring is a permanent `g_state` field,
because a log line — like a metric — has to survive across flushes rather than living on a
caller's stack for one operation. `SENTRY_MICRO_LOGS_ENABLED=0` removes it entirely:

```ini
build_flags = -D SENTRY_MICRO_LOGS_ENABLED=0
```

Measured on `esp32dev`, a build of `wifi_basic` that never calls `sentry_log()`, with and
without:

| | Enabled (default) | `SENTRY_MICRO_LOGS_ENABLED=0` | Saved |
| --- | --- | --- | --- |
| Flash | 941,557 B | 940,061 B | 1,496 B |
| RAM | 49,924 B | 49,092 B | 832 B |

`sentry_log()`, `sentry_logs_dropped_count()` and `sentry_logs_truncated_count()` are not
declared at all when disabled, the same as `set_ca_cert()` under `SENTRY_MICRO_WIFI_TLS=0`
above — a build that turns logs off and still tries to call one fails to compile rather than
silently doing nothing.

`SENTRY_MICRO_METRICS_ENABLED=0` does the same for Application Metrics (272 B RAM, ~1.1 KB
flash on the same build), and the two toggles combine: **1,104 B RAM and 3,056 B flash**
saved with both off.

## Writing a transport

Everything Sentry-specific has already happened by the time a transport is called: it gets a
URL, two headers, and a byte buffer. A complete implementation is a POST and a status check.

In C — one function pointer and a designated initialiser:

```c
static sentry_response_t my_send(void *ctx, const char *url, const sentry_headers_t *headers,
                                 const uint8_t *body, size_t len) {
    if (!my_post(url, headers->auth, headers->content_type, body, len)) {
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);  /* core buffers and retries */
    }
    return sentry_response_make(SENTRY_SEND_OK);
}

static sentry_transport_t my_transport = { .send = my_send };
sentry_set_transport(&my_transport);
```

`is_available` and `name` may be left NULL — an omitted availability check means "always
worth trying", not "never".

In C++ — subclass and override:

```cpp
class MyTransport : public sentry::Transport {
public:
    sentry::Response send(const char *url, const sentry::Headers &h,
                          const uint8_t *body, size_t len) override {
        return my_post(url, h.auth, body, len) ? sentry::SEND_OK : sentry::SEND_UNAVAILABLE;
    }
    const char *name() const override { return "mine"; }
};

static MyTransport transport;      // must outlive the SDK — not a stack local
sentry::set_transport(transport);
```

Both produce the same `sentry_transport_t` for the core to call; the C++ base class just
fills its function pointers with trampolines back to your virtuals.

`send()` returns a `Response`, but a bare result code converts to one, so a transport that
only knows "it worked" writes the lines above. A transport that *can* see response headers
should fill in more, because the core cannot invent it:

```cpp
return sentry::Response(sentry::SEND_RATE_LIMITED, 429, retry_after_seconds * 1000);
```

The struct exists so new response facts can be added without changing a signature every
transport implements — new fields default to zero, which the core reads as "no information".

`send()` may block. The core never calls it from `loop()` behind your back; see
[Delivery model](#delivery-model) for what that means in practice.

## Delivery model

`Transport::send()` is **blocking**, and the queuing/retry/backoff that makes blocking safe
lives in the core, above it. The alternative — an async transport interface with a completion
callback — pushes a task, a queue, and a buffer-ownership problem into every transport anyone
ever writes, to solve a problem that only has to be solved once.

What blocking actually costs on ESP32, measured against the Arduino core's own sdkconfig
rather than folklore:

| | Value | Consequence |
| --- | --- | --- |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S` | 5 | |
| `CONFIG_ESP_TASK_WDT_PANIC` | 1 | a watchdog timeout **reboots**, it does not just log |
| `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0` | set on ESP32/S3, **unset** on C3/S2 | |
| `CONFIG_ARDUINO_RUNNING_CORE` | 1 on ESP32/S3, 0 on C3/S2 | |
| `CONFIG_ARDUINO_LOOP_STACK_SIZE` | 8192 | the whole budget for a TLS handshake in `loop()` |

Reading those together: on dual-core parts the task watchdog watches the **CPU0** idle task
while `loop()` runs on **core 1**, and on single-core parts the idle task is not subscribed at
all. So a blocking `send()` in `loop()` does **not** trip the task watchdog out of the box on
any supported target. The real costs are different, and both are ordinary rather than dramatic:

1. **A multi-second stall in `loop()`.** A TLS POST is comfortably 2–5 s. On an LED controller
   that is a visible freeze — which is exactly why this belongs on its own task by default for
   anything with a render loop.
2. **Stack.** An mbedTLS handshake wants several KB, out of `loop()`'s 8 KB. It usually fits;
   it is tight, and a stack overflow is a panic.

The watchdog does become real in two cases: firmware that subscribes its own loop task
(`esp_task_wdt_add(NULL)`), and a handshake that starves a *subscribed* idle task. If it fires,
`TASK_WDT_PANIC=1` means a reboot — which this SDK would then dutifully report as a
`task_wdt` reset. Funny, but not a good look.

The upshot is that timing matters more than the interface. Boot-time crash reporting — read the
previous reset reason, send, carry on — can block freely, because nothing is animating yet. A
`capture_message()` from a render loop cannot. So the core will offer both: inline send
(default, no extra task, correct for the boot path) and an opt-in worker task for firmware that
cannot stall. A transport author writes the same ten lines either way.

## Supported targets

Every ESP32 family that runs the Arduino framework and has a WiFi radio:

| Target | Env | Platform |
| --- | --- | --- |
| ESP32 (Xtensa LX6) | `esp32dev` | `espressif32@7.0.1` |
| ESP32-S2 | `esp32-s2` | `espressif32@7.0.1` |
| ESP32-S3 | `esp32-s3` | `espressif32@7.0.1` |
| ESP32-C3 (RISC-V) | `esp32-c3` | `espressif32@7.0.1` |
| ESP32-C6 (RISC-V, WiFi 6) | `esp32-c6` | [`pioarduino`](https://github.com/pioarduino/platform-espressif32) fork — opt-in |

`pio run` in `examples/wifi_basic` builds the first four; C6 needs `pio run -e esp32-c6`
because the official PlatformIO platform does not ship Arduino support for it, so that env
pulls a separate community platform and a separate toolchain.

Not supported: **ESP32-H2** (802.15.4/BLE only — no WiFi) and **ESP8266** (different core, no
ESP-IDF coredump; it needs its own crash-capture path, tracked as a later extension).

### Partition table

The stock Arduino partition tables have no `coredump` partition, so a panic has nowhere to
write its dump and `esp_core_dump_get_summary()` has nothing to read on the next boot. The
example uses [`partitions/sentry-coredump-4mb.csv`](partitions/sentry-coredump-4mb.csv), which
is the stock 4 MB layout plus a 64 KB `coredump` partition. If you bring your own table, add
that partition.

## Development

```bash
pio test                                # host unit tests: the portable C core (native) and
                                         # the portable C++ transport routing (native_cxx)
cd examples/wifi_basic && pio run      # compile-check against every ESP32 variant
```

Building the example on all four variants is the portability gate — a change that breaks
RISC-V or single-core builds fails there rather than on someone's bench.

**Formatting runs in CI (`clang-format`, pinned to 22.1.8) and fails the build if a file
isn't formatted.** Catch it before pushing instead of after:

```bash
pip install clang-format==22.1.8   # exact version — brew's formula tracks upstream
                                    # latest and will eventually drift off this pin
brew install prek                  # or: pipx install prek — https://prek.j178.dev
prek install                       # one-time; also works with `pre-commit install`
```

This installs a git hook from [`.pre-commit-config.yaml`](.pre-commit-config.yaml) that
runs `scripts/format.sh` — the exact script and clang-format version CI uses — on the
C/C++ files in each `git commit`. Deliberately narrower than CI, which checks the whole
tree on every push: this only fails your commit over files *you* touched, not some
already-unformatted file elsewhere that CI would also catch on its own. If it reformats
anything, the commit is blocked; `git add -u` the reformatted files and commit again. Run
it on demand with `prek run --all-files`, or skip it for one commit with
`git commit --no-verify`. (Git hooks live in the repository, not the checkout —
installing from one `git worktree` of this repo enables it for all of them.)

**Touching anything that uses an Arduino API? Build C6 too:**

```bash
cd examples/wifi_basic && pio run -e esp32-c6
```

The four default envs are Arduino core **2.x** (IDF 4.4.7); C6 is core **3.x** (IDF 5.x) via
the pioarduino fork, and the two are not source-compatible. Core 3.x renamed the networking
classes, so `WiFiClient` there is a *typedef* for `NetworkClient` — a forward declaration of
it compiles fine on 2.x and is a conflicting definition on 3.x. CI catches this, but the C6
job is the slowest one, so it is cheaper to find locally.

## Roadmap

Implemented today:

- [x] DSN parsing → ingest URL + `X-Sentry-Auth` header, host-tested
- [x] Org id recovered from the DSN host, with an `Options::org_id` override for
      self-hosted (for trace propagation / the Dynamic Sampling Context)
- [x] Device context: chip model/revision/cores, eFuse device id, flash, heap, IDF version
- [x] `esp_reset_reason()` mapped to stable Sentry reset reasons, with a crash/not-crash split
- [x] Transport interface, in C with a C++ wrapper
- [x] Envelope + event JSON builder — fixed-buffer, no allocation, host-tested, and
      verified byte-for-byte on an ESP32-PICO-D4
- [x] Packaging + all-variant build

- [x] `WiFiTransport` — HTTPS POST straight to ingest, host-whitelisted against the DSN
- [x] Envelope accepted by production ingest (`HTTP 200`, event id echoed back)
- [x] `SerialTransport` + host relay script — a device with no network reporting through USB,
      validating the relay architecture the BLE transport will reuse
- [x] Offline buffering with NVS and filesystem backends, `Retry-After` backoff
- [x] Debug files uploaded and confirmed usable by Sentry (`symtab, debug, unwind`), indexed
      under the same `debug_id` the device reports
- [x] Core dumps read on the next boot and reported as an exception with a stacktrace
      (Xtensa: full backtrace. RISC-V: two frames, tagged `backtrace:truncated` — ESP-IDF
      does not unwind RISC-V, so a real trace needs server-side unwinding of the stack dump)
- [x] **Symbolicated end to end on hardware** — function, file, line and source, from a
      deliberate null dereference on an ESP32-PICO-D4

- [x] Generic relay protocol + `RelayTransport` — chunked binary framing for BLE-class links
- [x] `AutoTransport` — picks a route per delivery attempt from an ordered list of other
      transports, host-tested against fakes; replaces the hand-rolled `if (connected) ...
      else ...` `examples/wifi_basic` had
- [x] `capture_message()` for non-crash events, with a client-side throttle (repeat
      suppression plus a per-minute ceiling) so a message in a loop cannot exhaust the
      quota that crash reports come out of

Not done:


- [ ] **Sessions / release health** — crash-free rate per release across a fleet
- [ ] **WLED usermod** — the ready-made audience
- [ ] Full RISC-V backtraces (needs server-side unwinding of the stack dump)

Built but never exercised on hardware, which is worth knowing before trusting them:

- The RISC-V coredump reader compiles and matches the ESP-IDF struct, but has never run —
  there is no C-series board here.
- Every board except the classic ESP32. The S2, S3, C3 and C6 are covered by compilation
  only.

`WiFiTransport` delivery and the offline buffer surviving a power cycle *were* on this list;
both have since been confirmed on an ESP32-PICO-D4 over WiFi — see ONBOARDING.md.

## Onboarding

New to the project? [ONBOARDING.md](ONBOARDING.md) covers hardware, first event, first
symbolicated crash, and the traps that are not obvious — several of which fail silently.

Work is tracked in Linear:
[**sentry-micro**](https://linear.app/getsentry/project/sentry-micro-405057c524d4). Start with
[SDK-1407](https://linear.app/getsentry/issue/SDK-1407), which walks you from a clone to a
symbolicated crash on your own board before you change anything. The `Not done` list above is
a summary; Linear is the source of truth, and each issue carries the reasoning and the known
obstacles rather than just a title.

## Contributing

This is a prototype and the API will change. If you want ESP32 support in Sentry, the most
useful thing you can do is say so on
[sentry-native#915](https://github.com/getsentry/sentry-native/issues/915) — Sentry has
explicitly said the blocker is community signal, and a description of your deployment context
counts for more there than a +1.

## License

MIT — see [LICENSE](LICENSE).

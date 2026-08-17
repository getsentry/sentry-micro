<p align="center">
  <a href="https://sentry.io/?utm_source=github&utm_medium=logo" target="_blank">
    <img src="https://sentry-brand.storage.googleapis.com/sentry-wordmark-dark-280x84.png" alt="Sentry" width="280" height="84">
  </a>
</p>

# sentry-micro

**Sentry SDK for microcontrollers.** Captures crashes, reset reasons and device telemetry on
an ESP32 and delivers them to Sentry over *any* connectivity — WiFi directly, or by handing a
fully-formed Sentry envelope to a companion app that just relays the bytes.

> **Status: early prototype.** This is a Sentry hackweek project, not a released SDK. The
> build, the packaging and the portable core exist and are tested; event construction and the
> transports do not yet. See [Roadmap](#roadmap) for exactly what is and is not implemented,
> and [`ESP32_SENTRY_HACKWEEK.md`](ESP32_SENTRY_HACKWEEK.md) for the full proposal.

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
- **No filesystem.** Offline buffering targets NVS/flash directly.
- **The device builds the envelope; the transport is a dumb pipe.** Sentry's ingest protocol is
  SDK-agnostic — a plain `POST /api/<project>/envelope/` with an `X-Sentry-Auth` header. The
  library owns every bit of that; delivery is one virtual `send()` call behind
  [`sentry::Transport`](src/transport/sentry_transport.h). That is what lets the same core
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
    sentry_dsn.{h,c}        DSN parsing, ingest URL, auth header
    sentry_transport.{h,c}  the delivery interface
  device/                   chip-specific context collection
    sentry_device.h         the fields every event carries
    sentry_device_esp32.c   ESP-IDF implementation
  transport/
    sentry_transport.hpp    C++ base class over the C interface
test/                       host unit tests (Unity), run with `pio test -e native`
examples/
  wifi_basic/               a real sketch you can flash — WiFi + SDK init
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
    options.release = "my-firmware@1.0.0";   // must match your uploaded debug files
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
pio test -e native                     # host unit tests for the portable core
cd examples/wifi_basic && pio run      # compile-check against every ESP32 variant
```

Building the example on all four variants is the portability gate — a change that breaks
RISC-V or single-core builds fails there rather than on someone's bench.

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

Next, roughly in the order the [proposal](ESP32_SENTRY_HACKWEEK.md) lays out:

- [ ] `WiFiTransport` — HTTPS POST straight to ingest
- [ ] Coredump summary → native frames + `debug_meta` for server-side symbolication
- [ ] `RelayTransport` + the generic BLE relay protocol (host-whitelisted to the DSN host)
- [ ] Offline ring buffer in NVS, rate limiting, `429`/`Retry-After` backoff
- [ ] Breadcrumbs, sessions/release health, WLED usermod

## Contributing

This is a prototype and the API will change. If you want ESP32 support in Sentry, the most
useful thing you can do is say so on
[sentry-native#915](https://github.com/getsentry/sentry-native/issues/915) — Sentry has
explicitly said the blocker is community signal, and a description of your deployment context
counts for more there than a +1.

## License

MIT — see [LICENSE](LICENSE).

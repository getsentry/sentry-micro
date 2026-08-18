# Onboarding

Everything you need to get from a bare ESP32 to a symbolicated crash in Sentry, plus the
traps that cost time the first time round. Read [README.md](README.md) for what the SDK *is*;
this is how to work on it.

## What you need

**Any ESP32 will do**, but which one changes what you can test:

| Board | Arch | Notes |
| --- | --- | --- |
| ESP32 / ESP32-S2 / ESP32-S3 | Xtensa | Full backtraces. This is what everything was developed on. |
| ESP32-C3 / C6 | RISC-V | Two-frame backtraces only (see below). **Nobody has run this path yet** — if you have one of these, testing it is genuinely useful. |

Plus a USB cable, and — if you want to test the WiFi transport — **a 2.4 GHz network**. ESP32
has no 5 GHz radio. This bit us for the whole of the first week: the office network was not
reachable and every on-device delivery went through the serial relay instead.

```bash
brew install platformio clang-format           # or pip install platformio
brew install getsentry/tools/sentry-cli        # only for symbolication
```

## Zero to a first event

```bash
git clone https://github.com/getsentry/sentry-micro.git
cd sentry-micro

pio test -e native                             # 60 host tests, ~3s, no hardware
cd examples/wifi_basic && pio run               # compiles for 4 chip variants
```

Then put credentials in the environment rather than in a file — `scripts/env_secrets.py`
picks these up and compiles them in, so nothing sensitive ever lands in the repo:

```bash
export SENTRY_MICRO_DSN='https://<key>@o<org>.ingest.<region>.sentry.io/<project>'
export SENTRY_MICRO_WIFI_SSID='...'
export SENTRY_MICRO_WIFI_PASSWORD='...'
pio run -e esp32dev -t upload
```

**No 2.4 GHz network?** Use the serial relay — your laptop performs the HTTP request on the
device's behalf. This is also the fastest way to see exactly what the device is sending:

```bash
scripts/serial_relay.py --port /dev/cu.usbserial-XXXX     # replaces `pio device monitor`
```

## Zero to a symbolicated crash

```bash
scripts/release.sh -e esp32dev -r 'my-firmware@0.1.0'     # build, stamp, upload debug files
```

Needs `SENTRY_AUTH_TOKEN` (an **organization** auth token, scope `org:ci`). Org and project
are read out of the DSN. Then flash a build with the deliberate crash enabled:

```bash
PLATFORMIO_BUILD_FLAGS='-D SENTRY_DEMO_CRASH=1' scripts/release.sh -e esp32dev -r 'demo@0.1.0'
# then flash it, exporting the SENTRY_MICRO_* values release.sh printed
```

The board crashes once per power-on, reboots, reports the crash on the next boot, and erases
the core dump once it is delivered.

Reflashing to get one crash costs a minute at 115200 baud. To crash on demand instead, build
with a button pin — `-D SENTRY_CRASH_BUTTON_PIN=39` is the M5Stack's left-hand button — and
press it whenever you want the loop to run again. On a bare devkit note that GPIO 34-39 are
input-only with no internal pull-up, so that pin needs an external pull-up or it floats and
crashes at random.

## Traps

Each of these cost real time. None of them announce themselves.

**`-Wl,--build-id` bricks the boot.** The obvious way to get a GNU build-id into the ELF marks
the note `ALLOC` and places it at the start of IRAM, pushing `.iram0.vectors` off its
1 KB-aligned base. The first interrupt jumps into the note and the board boot-loops with
`rst:0x10 (RTCWDT_RTC_RESET)`. `scripts/stamp_build_id.py` attaches the note *after* linking as
a non-`ALLOC` section instead. Do not "simplify" this back to the flag.

**Symbolication fails silently.** When `image_addr` was wrong, the device said delivered, ingest
returned `200`, `sentry-cli` said `Usable: yes`, and the `debug_id` matched — and every frame
still rendered `<unknown>`. There is no error anywhere in that chain. The only way to know
symbolication works is to look at an issue in the UI. Do that after any change to
`debug_meta`, the release script, or the linker layout.

**Arduino core 2.x and 3.x are not source-compatible.** The four default envs are core 2.x
(IDF 4.4.7); `esp32-c6` is core 3.x (IDF 5.x) via the pioarduino fork. Core 3.x renamed the
networking classes — `WiFiClient` there is a *typedef* for `NetworkClient`, so a forward
declaration compiles on one and is a conflicting definition on the other.

**RISC-V core dumps are a different structure, not different field names.** Xtensa gives
`exc_cause`/`exc_vaddr` and a backtrace ESP-IDF has already unwound. RISC-V gives
`mcause`/`mtval` and a raw stack dump with no unwinding, because it has no fixed frame layout
to walk without DWARF.

**Build all five variants before claiming anything works.** Both of the above were shipped
broken because only the four default envs were built. `pio run` covers four;
`pio run -e esp32-c6` is the fifth and is not in `default_envs` because it pulls a separate
toolchain.

**A scan that is too thorough always fails.** Arduino's `scanNetworks()` waits a hard-coded
10s and returns `-2` if the scan has not finished, whatever dwell you asked it for. An active
scan visits ~13 channels, so anything above ~750ms per channel can never succeed — and the
failed call leaves `WIFI_SCANNING_BIT` set, so every retry afterwards returns `-1`
("still running") and no amount of resetting the radio clears it. The diagnostic that exists
to tell "wrong password" from "5 GHz network" was broken this way and failed 100% of the time.

**The image measurements move when you bake them in.** `release.sh` compiles `image_addr` and
`image_size` into the firmware, which relinks, which changes the size — on Xtensa a large
constant moves from a two-byte `movi.n` to a four-byte literal-pool entry. Two passes are not
always enough, so the script iterates to a fixpoint and refuses to ship if it never settles.
A firmware reporting the previous pass's size is exactly the silent-symbolication-failure
class of bug above.

**Some USB-UART bridges cannot renegotiate baud.** On the M5Stack (FTDI FT232R) both 921600
and 460800 die with "Unable to verify flash chip connection" immediately after esptool reports
"Changed"; 115200 works. Hence `upload_speed = 115200` on `esp32dev`.

## Working on it

```bash
pio test -e native            # host tests: DSN, envelope, buffer, relay protocol
scripts/format.sh             # clang-format 22.1.8, pinned; --check in CI
cd examples/wifi_basic && pio run && pio run -e esp32-c6
```

The `core/` vs `device/` split is load-bearing: everything in `core/` is freestanding C that
compiles on a laptop, which is why the wire format, the ring buffer and the relay framing are
all testable in milliseconds without hardware. Put anything portable there and it gets tests
for free. Anything that touches ESP-IDF goes in `device/`.

CI runs the host tests, a format check, and all five variant builds on every push.

## What is not verified

Written and tested, never actually run on hardware — check before trusting:

- **The whole RISC-V core dump reader.** Compiles, matches the ESP-IDF struct, never executed.
  This is the big one: no C3 or C6 has ever run this code. If you have one of those boards,
  start here (SDK-1388).
- **Every board except the classic ESP32.** The S2, S3, C3 and C6 are covered by compilation
  only. All five variants build; only `esp32dev` has been flashed.
- **TLS certificate verification.** `WiFiTransport` runs with verification off unless you call
  `set_ca_cert()`, and nobody has yet run it with a real certificate pinned.

Verified on hardware (ESP32-PICO-D4 on an M5Stack, over WiFi):

- **`WiFiTransport` delivers.** Boot events and crash reports both POST straight to ingest and
  come back `200`.
- **Crash -> reboot -> report -> symbolicate.** A null-pointer store three calls deep is
  recovered from the core dump partition on the next boot and reported with all five frames.
- **The buffer survives a reboot.** Two envelopes written with no network reachable were
  recovered from NVS across resets *and* across a reflash, then flushed once the radio came
  up: `flushed 2 envelopes, 0 still waiting`.

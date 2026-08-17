#!/usr/bin/env python3
"""
Relay Sentry envelopes from a USB-connected device to the internet.

The device has no network; the machine it is plugged into does. This reads the serial port,
passes ordinary log output through to your terminal, and when the device emits a relay
request it performs the HTTPS POST on the device's behalf and reports the status back.

    scripts/serial_relay.py --port /dev/cu.usbserial-XXXX

It is a stand-in for `pio device monitor` — both cannot own the port at once.

WHAT THIS IS REALLY FOR
-----------------------
It is the same architecture as the BLE relay in the proposal: the *device* builds a complete
Sentry request, and something else moves the bytes. This relay knows nothing about Sentry
beyond "POST these bytes to this URL", which is exactly the property that lets a companion
app support the SDK in ~20 lines. Proving it over a cable is much easier to debug than
proving it over BLE, and the device-side code is identical either way.

SECURITY
--------
The device asks this script to POST arbitrary bytes to an arbitrary URL. A buggy or hostile
device could otherwise use your machine as an open proxy, so the host is whitelisted against
the DSN and everything else is refused. This is non-negotiable in the BLE case — where the
relay is a user's phone — and the rule is enforced identically here.

PROTOCOL (must match src/transport/sentry_transport_serial.hpp)
---------------------------------------------------------------
    device -> host   @SENTRY-RELAY/1 <b64 url> <b64 auth> <b64 content-type> <b64 body>
    host -> device   @SENTRY-RELAY-RESULT/1 <http-status> <retry-after-ms>

Every field is base64 because URLs and auth headers contain spaces and an envelope is ndjson
that contains newlines; encoding removes all framing and escaping questions.
A status of 0 means the host could not reach Sentry at all, which the device reads as
"no route" rather than as a rejection.
"""

import argparse
import base64
import os
import sys
import time
import urllib.error
import urllib.request
from urllib.parse import urlparse

try:
    import serial  # pyserial
except ImportError:
    sys.exit(
        "pyserial is required.\n"
        "  pip install pyserial\n"
        "or use PlatformIO's copy: ~/.platformio/penv/bin/python scripts/serial_relay.py ..."
    )

REQUEST_PREFIX = "@SENTRY-RELAY/1 "
RESULT_PREFIX = "@SENTRY-RELAY-RESULT/1 "

# A relayed envelope is ~1KB of base64; this is generous while still bounding a runaway line.
MAX_LINE_BYTES = 256 * 1024


def dsn_host(dsn: str) -> str:
    """Ingest hostname from a DSN. This is the only host the relay will ever POST to."""
    parsed = urlparse(dsn)
    if not parsed.hostname:
        raise ValueError(f"could not parse a hostname out of the DSN: {dsn!r}")
    return parsed.hostname


def perform_post(url: str, auth: str, content_type: str, body: bytes, allowed_host: str,
                 timeout: float):
    """
    POST `body` to `url`, but only if `url` is on the whitelisted host.

    Returns (http_status, retry_after_ms). A status of 0 means the request never reached a
    server; -1 means it was refused locally and must not be retried.
    """
    parsed = urlparse(url)
    # Exact match, and https only. Suffix matching would accept `sentry.io.evil.com`, and
    # allowing http would let a downgrade strip the transport security entirely.
    if parsed.scheme != "https" or parsed.hostname != allowed_host:
        print(f"[relay] REFUSED {url} (allowed host is {allowed_host} over https)",
              file=sys.stderr)
        return -1, 0

    request = urllib.request.Request(url, data=body, method="POST")
    request.add_header("Content-Type", content_type)
    request.add_header("X-Sentry-Auth", auth)

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, retry_after_ms(response.headers)
    except urllib.error.HTTPError as error:
        # A 4xx/5xx is a real answer from Sentry and the device needs to see it verbatim,
        # so this is not an error from the relay's point of view.
        return error.code, retry_after_ms(error.headers)
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        # The *host* has no route. Reported as 0 so the device buffers and retries rather
        # than treating its own perfectly good envelope as rejected.
        print(f"[relay] network error: {error}", file=sys.stderr)
        return 0, 0


def retry_after_ms(headers) -> int:
    """Seconds from `Retry-After`, or the leading field of `X-Sentry-Rate-Limits`."""
    value = headers.get("Retry-After")
    if value:
        try:
            return max(0, int(float(value))) * 1000
        except ValueError:
            pass  # HTTP-date form; the device falls back to its own backoff.
    limits = headers.get("X-Sentry-Rate-Limits")
    if limits:
        try:
            return max(0, int(limits.split(":", 1)[0].split(",")[0])) * 1000
        except (ValueError, IndexError):
            pass
    return 0


def handle_request(line: str, port, allowed_host: str, timeout: float) -> None:
    fields = line[len(REQUEST_PREFIX):].split()
    if len(fields) != 4:
        print(f"[relay] malformed request ({len(fields)} fields, expected 4)", file=sys.stderr)
        return

    try:
        url, auth, content_type, body = (base64.b64decode(f, validate=True) for f in fields)
    except Exception as error:
        print(f"[relay] could not decode request: {error}", file=sys.stderr)
        return

    url = url.decode("utf-8", "replace")
    auth = auth.decode("utf-8", "replace")
    content_type = content_type.decode("utf-8", "replace")

    print(f"[relay] {len(body)} bytes -> {url}", file=sys.stderr)
    started = time.monotonic()
    status, retry_ms = perform_post(url, auth, content_type, body, allowed_host, timeout)
    elapsed_ms = int((time.monotonic() - started) * 1000)
    print(f"[relay] HTTP {status} in {elapsed_ms}ms", file=sys.stderr)

    port.write(f"{RESULT_PREFIX}{status} {retry_ms}\n".encode("ascii"))
    port.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--port", required=True, help="serial device, e.g. /dev/cu.usbserial-X")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--dsn",
        default=os.environ.get("SENTRY_MICRO_DSN"),
        help="DSN whose host is the only one this relay will POST to "
             "(defaults to $SENTRY_MICRO_DSN)",
    )
    parser.add_argument("--timeout", type=float, default=10.0, help="HTTP timeout in seconds")
    args = parser.parse_args()

    if not args.dsn:
        return parser.error("no DSN: pass --dsn or set SENTRY_MICRO_DSN")

    allowed_host = dsn_host(args.dsn)
    print(f"[relay] listening on {args.port} @ {args.baud}", file=sys.stderr)
    print(f"[relay] will POST only to https://{allowed_host}/", file=sys.stderr)

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        buffer = bytearray()
        while True:
            chunk = port.read(4096)
            if not chunk:
                continue
            buffer.extend(chunk)

            while b"\n" in buffer:
                raw, _, rest = buffer.partition(b"\n")
                buffer = bytearray(rest)
                line = raw.decode("utf-8", "replace").rstrip("\r")

                if line.startswith(REQUEST_PREFIX):
                    handle_request(line, port, allowed_host, args.timeout)
                else:
                    # Ordinary device output: pass it through so this can replace the monitor.
                    print(line, flush=True)

            if len(buffer) > MAX_LINE_BYTES:
                print("[relay] dropping an over-long line", file=sys.stderr)
                buffer.clear()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)

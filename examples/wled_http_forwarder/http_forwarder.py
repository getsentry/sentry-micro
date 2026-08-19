#!/usr/bin/env python3
"""
Relay plain HTTP requests from a device to a real HTTPS endpoint.

Runs continuously on the LAN so a WiFi-connected device can push to it the moment it
reconnects, with no browser and no polling involved. Compare to sentry-micro's
scripts/serial_relay.py, which makes one HTTPS POST on a USB-tethered device's behalf
instead.

Give it your project's real DSN; it derives everything else and prints the DSN to put on
the device:

    http_forwarder.py --dsn https://abc123@o123456.ingest.us.sentry.io/789

WHY IT ONLY NEEDS YOUR REAL DSN
--------------------------------
A Sentry DSN is `<scheme>://<public_key>@<host>[:<port>]/<project_id>` (see
src/core/sentry_dsn.h), and sentry-micro's WiFiTransport builds the ingest URL purely from
the DSN it's given — nothing else in the SDK cares what that DSN's host actually resolves
to. So this script takes your real DSN, keeps its public_key and project_id, and swaps in
its own address as the host — that's the DSN to put on the device. The host it keeps for
itself is where it actually relays to, which is also why the device never needs to know or
guess it: DSN hosts are region-specific (`ingest.us.sentry.io` vs. `ingest.de.sentry.io`,
or your own domain if self-hosted), and getting that wrong from the device side would fail
silently — the forwarder just re-uses the host your real DSN already had.

SECURITY
--------
Unlike serial_relay.py, this does NOT whitelist against a DSN host, because there is no
DSN in the request it receives to check against — the whole point is that the device never
sees the real host. It forwards to exactly one host, over real certificate-verified HTTPS,
and does nothing else. Only run this with a DSN for a project you trust.

This is deliberately a throwaway example tool, not a production forwarder meant to run
unattended long-term.
"""

import argparse
import http.client
import socket
import ssl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

# Headers that are per-hop and must not be replayed onto the new connection — the new
# connection needs its own Host and framing, and gzip round-tripped byte-for-byte through
# a script that isn't decompressing it is more likely to hide a mistake than reveal one.
HOP_BY_HOP_HEADERS = frozenset({
    "host",
    "content-length",
    "connection",
    "transfer-encoding",
    "accept-encoding",
})


def parse_dsn(dsn: str):
    """Split a DSN into (public_key, host, port, project_path).

    project_path is everything after host[:port] with the leading slash stripped — a bare
    project id for sentry.io, or a longer path first for a self-hosted install with a
    URL prefix. Kept as one opaque string rather than assuming it's always numeric.
    """
    parts = urlsplit(dsn)
    if parts.scheme not in ("http", "https") or not parts.hostname or not parts.username:
        raise ValueError(f"not a valid DSN: {dsn!r}")
    project_path = parts.path.lstrip("/")
    if not project_path:
        raise ValueError(f"DSN is missing a project id: {dsn!r}")
    return parts.username, parts.hostname, parts.port or 443, project_path


def detect_lan_ip() -> str:
    """Best-effort local IP a device on the same LAN could reach this machine on.

    Opens a UDP socket toward a public address without sending anything — UDP `connect()`
    only asks the kernel to pick a route, which is enough to read back the local address
    that route would use. Falls back to loopback if there's no route at all (offline).
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def make_handler(upstream_host: str, upstream_port: int):
    class ForwarderHandler(BaseHTTPRequestHandler):
        def do_POST(self):  # noqa: N802 (BaseHTTPRequestHandler's naming)
            content_length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(content_length) if content_length else b""

            forward_headers = {
                key: value
                for key, value in self.headers.items()
                if key.lower() not in HOP_BY_HOP_HEADERS
            }
            forward_headers["Host"] = upstream_host

            print(f"-> {self.path} ({len(body)} bytes) -> https://{upstream_host}{self.path}")

            try:
                conn = http.client.HTTPSConnection(
                    upstream_host, upstream_port, context=ssl.create_default_context(), timeout=10
                )
                conn.request("POST", self.path, body=body, headers=forward_headers)
                upstream_response = conn.getresponse()
                response_body = upstream_response.read()
                conn.close()
            except OSError as exc:
                print(f"<- forwarding failed: {exc}")
                self.send_response(502)
                self.end_headers()
                return

            print(f"<- {upstream_response.status} ({len(response_body)} bytes)")

            self.send_response(upstream_response.status)
            for key, value in upstream_response.getheaders():
                if key.lower() not in HOP_BY_HOP_HEADERS:
                    self.send_header(key, value)
            self.end_headers()
            if response_body:
                self.wfile.write(response_body)

        def log_message(self, format, *args):  # noqa: A002 (matches base class signature)
            # Replaced by the print()s above, which show the information that actually
            # matters here (forwarded path, byte counts, upstream status).
            pass

    return ForwarderHandler


def main():
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dsn", required=True,
        help="Your project's real DSN, e.g. https://abc123@o123456.ingest.us.sentry.io/789")
    parser.add_argument("--listen-port", type=int, default=8080,
        help="Plain-HTTP port to listen on (default: 8080)")
    parser.add_argument("--advertise-host",
        help="LAN address to print in the device DSN, if auto-detection picks the wrong "
             "network interface")
    args = parser.parse_args()

    public_key, upstream_host, upstream_port, project_path = parse_dsn(args.dsn)
    advertise_host = args.advertise_host or detect_lan_ip()
    device_dsn = f"http://{public_key}@{advertise_host}:{args.listen_port}/{project_path}"

    print(f"relaying to https://{upstream_host}:{upstream_port}")
    print("device SENTRY_DSN:")
    print(f"    {device_dsn}")

    handler = make_handler(upstream_host, upstream_port)
    server = ThreadingHTTPServer(("0.0.0.0", args.listen_port), handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

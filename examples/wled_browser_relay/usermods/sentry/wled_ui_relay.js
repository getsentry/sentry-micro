
// ── Sentry relay ────────────────────────────────────────────────────────────────────────
//
// Relay crash reports for a device that cannot send them itself.
//
// WLED has no TLS stack and Sentry's ingest is HTTPS-only, so the firmware never gets a
// route to Sentry at all. Instead it builds the whole request — URL, two header values,
// envelope body — and hands it out in frames over HTTP; this performs the request and
// reports the status back. The browser already has a TLS stack, a CA store and an internet
// connection, and this borrows all three for the length of one POST.
//
// It lives here, in the UI, rather than on a page of its own, because the point is that
// nobody has to know it exists. Open WLED the way you always do and the reports go out.
//
// Nothing here knows what a Sentry envelope is. Everything Sentry-specific — the DSN, the
// envelope format, the auth header, the ingest URL, rate-limit backoff — is built on the
// device by sentry-micro. This is a dumb pipe with a whitelist, and it is deliberately the
// whole of the host side: see core/sentry_relay.h for the wire protocol.
//
// INERT WITHOUT THE USERMOD. The first status fetch is the feature check: a build without
// the sentry usermod answers 404, and this disables itself permanently and silently. It
// costs one request on load and nothing after that.
//
// Wire protocol (device -> host), little-endian:
//   0x01 BEGIN [req u8][urlLen u16][authLen u16][ctypeLen u16][bodyLen u32]
//   0x02 DATA  [req u8][offset u16][payload...]
//   0x03 END   [req u8]
// DATA payloads are slices of one stream: url ++ auth ++ contentType ++ body, cut apart
// using the lengths from BEGIN.

(function () {
	"use strict";

	var FRAME_BEGIN = 0x01, FRAME_DATA = 0x02, FRAME_END = 0x03;
	var PROTOCOL_VERSION = 1;

	// How much of one request the device may put in a single DATA frame. On a BLE link this
	// is the negotiated MTU; over HTTP it is only a memory/round-trip trade.
	var MAX_CHUNK = 1024;

	// A relayed request is an envelope, never an upload. Anything larger is a bug or an
	// abuse, and this browser is the thing with a network to abuse.
	var MAX_REQUEST_BYTES = 64 * 1024;

	var RESULT_OK = 0, RESULT_UNAVAILABLE = 1, RESULT_REJECTED = 2,
	    RESULT_RATE_LIMITED = 3, RESULT_ERROR = 4;

	// Idle polling is deliberately slow. This runs for as long as somebody has WLED open,
	// on a device whose actual job is rendering an LED strip, and a crash report is worth
	// nothing extra for arriving four seconds sooner. It speeds up only while a request is
	// mid-flight, where the delay is real latency rather than patience.
	var IDLE_POLL_MS = 5000;
	var ACTIVE_POLL_MS = 500;

	var allowedHost = null;   // the one host we will POST to; from the device's own DSN
	var pending = null;       // the request being assembled, or null
	var timer = null;

	function log(msg)  { console.log("[sentry] " + msg); }
	function warn(msg) { console.warn("[sentry] " + msg); }

	function b64ToBytes(s) {
		var bin = atob(s), out = new Uint8Array(bin.length);
		for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
		return out;
	}

	// `Retry-After` is seconds (or an HTTP date); the device wants milliseconds.
	function retryAfterMs(response) {
		var header = response.headers.get("Retry-After");
		if (!header) return 0;
		var seconds = Number(header);
		if (isFinite(seconds)) return Math.max(0, Math.round(seconds * 1000));
		var date = Date.parse(header);
		return isNaN(date) ? 0 : Math.max(0, date - Date.now());
	}

	function sendStatusFrame(reqId, result, httpStatus, retry) {
		var q = "?req=" + reqId + "&result=" + result +
		        "&http=" + (httpStatus || 0) + "&retry=" + (retry || 0);
		// The device times out on its own and keeps the event buffered, so a lost status
		// costs a retry rather than the report.
		return fetch("/sentry/relay/status" + q, { method: "POST" })
			.catch(function (err) { warn("could not report status to device: " + err); });
	}

	function performRequest(req) {
		var dec = new TextDecoder(), at = 0;
		var url   = dec.decode(req.buf.subarray(at, (at += req.urlLen)));
		var auth  = dec.decode(req.buf.subarray(at, (at += req.authLen)));
		var ctype = dec.decode(req.buf.subarray(at, (at += req.ctypeLen)));
		var body  = req.buf.subarray(at, at + req.bodyLen);

		var parsed;
		try { parsed = new URL(url); }
		catch (err) { return sendStatusFrame(req.reqId, RESULT_REJECTED); }

		// The whitelist, and it is not optional. Without it a bug in the firmware — or a
		// device somebody else put on this network — could use this browser as an open
		// proxy, carrying the user's IP and cookies to any URL it named. The device's own
		// RelayTransport enforces the same rule on its side; neither end depends on the
		// other having got it right. `username` is checked because
		// https://ingest.sentry.io@evil.com/ has a host of evil.com but reads like the real
		// thing to a human skimming a log.
		if (parsed.protocol !== "https:" || parsed.host.toLowerCase() !== allowedHost ||
		    parsed.username) {
			warn("refusing to POST to " + parsed.host + " (allowed: " + allowedHost + ")");
			return sendStatusFrame(req.reqId, RESULT_REJECTED);
		}

		return fetch(url, {
			method: "POST",
			headers: { "X-Sentry-Auth": auth, "Content-Type": ctype },
			body: body
		}).then(function (response) {
			var result = RESULT_ERROR;
			if (response.ok) result = RESULT_OK;
			else if (response.status === 429) result = RESULT_RATE_LIMITED;
			else if (response.status >= 400 && response.status < 500) result = RESULT_REJECTED;
			log("relayed " + body.byteLength + " B -> " + response.status);
			return sendStatusFrame(req.reqId, result, response.status, retryAfterMs(response));
		}, function (err) {
			// No network on this side either. UNAVAILABLE tells the device to keep the event
			// buffered and try again rather than treating it as delivered.
			warn("relay failed: " + err);
			return sendStatusFrame(req.reqId, RESULT_UNAVAILABLE);
		});
	}

	function onFrame(f) {
		if (f.length < 2) return;
		var dv = new DataView(f.buffer, f.byteOffset, f.byteLength);
		var type = dv.getUint8(0), reqId = dv.getUint8(1);

		if (type === FRAME_BEGIN) {
			if (f.length < 12) return;
			var urlLen = dv.getUint16(2, true), authLen = dv.getUint16(4, true);
			var ctypeLen = dv.getUint16(6, true), bodyLen = dv.getUint32(8, true);
			var total = urlLen + authLen + ctypeLen + bodyLen;
			if (total === 0 || total > MAX_REQUEST_BYTES) {
				pending = null;
				sendStatusFrame(reqId, RESULT_REJECTED);
				return;
			}
			pending = { reqId: reqId, urlLen: urlLen, authLen: authLen, ctypeLen: ctypeLen,
			            bodyLen: bodyLen, buf: new Uint8Array(total), received: 0, seen: {} };
			return;
		}

		if (!pending || pending.reqId !== reqId) return;

		if (type === FRAME_DATA) {
			if (f.length <= 4) return;
			var offset = dv.getUint16(2, true), n = f.length - 4;
			if (offset + n > pending.buf.length) return;   // overrun: drop the frame
			pending.buf.set(f.subarray(4), offset);
			// Count coverage by offset rather than a running total, so a retransmitted chunk
			// cannot make a request with a hole in it look complete.
			if (!pending.seen[offset]) { pending.seen[offset] = true; pending.received += n; }
			return;
		}

		if (type === FRAME_END) {
			var req = pending;
			pending = null;
			if (req.received < req.buf.length) {
				warn("incomplete request (" + req.received + "/" + req.buf.length + " B)");
				sendStatusFrame(reqId, RESULT_ERROR);   // retryable: the device still has it
				return;
			}
			performRequest(req);
		}
	}

	function poll() {
		// The HELLO frame is folded into the query string rather than sent as its own
		// round-trip: on this link every poll is already the host announcing itself, and
		// doing it this way means a device that rebooted — which is what every crash does —
		// re-attaches on the next poll with no reconnect logic on either side.
		return fetch("/sentry/relay/poll?v=" + PROTOCOL_VERSION + "&chunk=" + MAX_CHUNK,
		             { cache: "no-store" })
			.then(function (res) {
				if (!res.ok) throw new Error("HTTP " + res.status);
				return res.text();
			})
			.then(function (text) {
				text = text.trim();
				if (!text) return;
				// The device concatenates its queued frames as [len u16][frame...] repeated,
				// base64 of the whole thing. HTTP hands over a byte stream with no packet
				// boundaries in it, so the link supplies its own — the job a BLE notification
				// does for free on the other transport.
				var bytes = b64ToBytes(text), off = 0;
				while (off + 2 <= bytes.length) {
					var len = bytes[off] | (bytes[off + 1] << 8);
					off += 2;
					if (off + len > bytes.length) break;
					onFrame(bytes.subarray(off, off + len));
					off += len;
				}
			})
			.catch(function () {
				// Device rebooting, WLED restarting, network blip. Nothing to do: the device
				// keeps everything buffered in flash and the next poll picks it up.
				pending = null;
			});
	}

	// setTimeout rather than setInterval, chained after the previous poll settles. A poll
	// that outlives its interval — a slow POST to Sentry sits inside it — would otherwise
	// overlap with the next one, and both would drain the device's queue, with responses
	// arriving out of order and DATA frames landing before the BEGIN that sizes the buffer
	// for them.
	function schedule() {
		timer = setTimeout(function () {
			poll().then(function () {
				schedule();
			});
		}, pending ? ACTIVE_POLL_MS : IDLE_POLL_MS);
	}

	// The feature check. A WLED build without the sentry usermod has no such route, answers
	// 404, and this returns without scheduling anything — so the cost of carrying this code
	// on a build that cannot use it is one request at page load.
	fetch("/sentry/status", { cache: "no-store" })
		.then(function (res) {
			if (!res.ok) throw new Error("HTTP " + res.status);
			return res.json();
		})
		.then(function (st) {
			allowedHost = (st.host || "").toLowerCase() || null;
			if (!st.enabled || !allowedHost) {
				warn("usermod present but not reporting (no valid DSN); relay idle");
				return;
			}
			log("relaying for this device to " + allowedHost);
			schedule();
		})
		.catch(function () { /* no usermod on this build; stay silent and do nothing */ });
})();

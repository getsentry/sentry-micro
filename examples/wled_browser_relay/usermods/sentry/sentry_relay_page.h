/*
 * The relay page — the host side of the relay protocol, in a browser tab.
 *
 * A WLED device has no TLS stack and no route to Sentry. It does, however, serve a web UI
 * to a browser that has both. This page is the other end of sentry-micro's relay protocol
 * (see core/sentry_relay.h): it pulls the finished request the device has queued — URL,
 * two header values, envelope body — performs it with `fetch()`, and reports the HTTP
 * status back. Nothing here knows what an envelope is; the device owns every Sentry
 * detail, and this is a dumb pipe with a status display attached.
 *
 * That makes it the direct analogue of a companion phone app relaying over BLE, which is
 * the case the protocol was designed for. The link is different (HTTP polling instead of
 * GATT notifications); the frames on it are identical.
 *
 * Served as one string from PROGMEM rather than from the filesystem: a usermod that needs
 * files uploaded to LittleFS before it works is a usermod nobody will try.
 *
 * Kept deliberately dependency-free and un-minified — this is the file a WLED developer
 * reads to decide whether the design is reasonable, so it optimises for that over bytes.
 */
#ifndef SENTRY_RELAY_PAGE_H
#define SENTRY_RELAY_PAGE_H

#include <Arduino.h>

static const char SENTRY_RELAY_PAGE[] PROGMEM = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WLED &middot; Sentry relay</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; padding:1.25rem; background:#181818; color:#e8e8e8;
         font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif; }
  h1 { font-size:1.1rem; margin:0 0 .25rem; font-weight:600; }
  .sub { color:#9a9a9a; margin:0 0 1.25rem; }
  .card { background:#222; border:1px solid #333; border-radius:8px; padding:1rem; margin-bottom:1rem; }
  .row { display:flex; justify-content:space-between; gap:1rem; padding:.2rem 0; }
  .row span:first-child { color:#9a9a9a; }
  .row span:last-child { font-family:ui-monospace,SFMono-Regular,Menlo,monospace; text-align:right;
                          word-break:break-all; }
  .dot { display:inline-block; width:.55rem; height:.55rem; border-radius:50%; margin-right:.45rem; }
  .ok { background:#3ecf8e; } .warn { background:#e8b339; } .bad { background:#e5484d; }
  button { background:#2f2f2f; color:#e8e8e8; border:1px solid #444; border-radius:6px;
           padding:.5rem .9rem; font-size:.9rem; cursor:pointer; margin-right:.5rem; }
  button:hover { background:#3a3a3a; }
  #log { background:#141414; border:1px solid #2c2c2c; border-radius:6px; padding:.75rem;
         height:16rem; overflow-y:auto; font-family:ui-monospace,SFMono-Regular,Menlo,monospace;
         font-size:12px; white-space:pre-wrap; }
  #log div { padding:.05rem 0; }
  .t { color:#6b6b6b; } .e { color:#e5484d; } .g { color:#3ecf8e; } .y { color:#e8b339; }
</style>
</head>
<body>
<h1>Sentry relay</h1>
<p class="sub">Leave this tab open. Crash reports the device cannot send itself are relayed
through this browser.</p>

<div class="card">
  <div class="row"><span>Relay</span><span id="s-relay"><span class="dot warn"></span>starting</span></div>
  <div class="row"><span>Ingest host</span><span id="s-host">&mdash;</span></div>
  <div class="row"><span>Release</span><span id="s-release">&mdash;</span></div>
  <div class="row"><span>Coredump</span><span id="s-coredump">&mdash;</span></div>
  <div class="row"><span>Queued on device</span><span id="s-buffered">&mdash;</span></div>
  <div class="row"><span>Relayed this session</span><span id="s-sent">0</span></div>
</div>

<div class="card">
  <button id="b-test">Send a test event</button>
  <button id="b-crash">Crash the device</button>
</div>

<div id="log"></div>

<script>
"use strict";

/* Mirrors core/sentry_relay.h. Kept as literals rather than fetched from the device: the
   host is what pins the protocol version, and a host that silently agreed with whatever
   the device claimed would defeat the point of announcing one. */
var FRAME_BEGIN = 0x01, FRAME_DATA = 0x02, FRAME_END = 0x03;
var PROTOCOL_VERSION = 1;

/* How much of one request the device may put in a single DATA frame. On a BLE link this is
   the negotiated MTU; here it is only a memory/round-trip trade, so it is generous. */
var MAX_CHUNK = 1024;

/* A relayed request is an envelope, never an upload. Anything larger is a bug or an abuse,
   and this browser is the thing with a network to abuse. */
var MAX_REQUEST_BYTES = 64 * 1024;

var RESULT_OK = 0, RESULT_UNAVAILABLE = 1, RESULT_REJECTED = 2,
    RESULT_RATE_LIMITED = 3, RESULT_ERROR = 4;

var POLL_MS = 1000;

var allowedHost = null;   /* the one host we will POST to; from the device's own DSN */
var pending = null;       /* the request being assembled, or null */
var relayed = 0;

function log(msg, cls) {
  var box = document.getElementById("log");
  var line = document.createElement("div");
  var now = new Date().toTimeString().slice(0, 8);
  line.innerHTML = '<span class="t">' + now + "</span>  ";
  var body = document.createElement("span");
  if (cls) body.className = cls;
  body.textContent = msg;
  line.appendChild(body);
  box.appendChild(line);
  /* Only follow the tail when the reader is already at it, so scrolling back to read
     something does not get yanked away by the next line. */
  if (box.scrollHeight - box.scrollTop - box.clientHeight < 40) box.scrollTop = box.scrollHeight;
}

function setRelayState(cls, text) {
  document.getElementById("s-relay").innerHTML =
    '<span class="dot ' + cls + '"></span>' + text;
}

function b64ToBytes(s) {
  var bin = atob(s), out = new Uint8Array(bin.length);
  for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

/* ---- status ------------------------------------------------------------------------ */

function renderStatus(st) {
  allowedHost = (st.host || "").toLowerCase() || null;
  document.getElementById("s-host").textContent = st.host || "(no DSN)";
  document.getElementById("s-release").textContent = st.release || "-";
  document.getElementById("s-buffered").textContent = st.buffered;
  document.getElementById("s-coredump").textContent =
    st.coredumpSupported ? (st.coredumpAvailable ? "supported, one waiting" : "supported")
                         : "not supported by this build";
  if (!st.enabled) setRelayState("bad", "SDK disabled - check the DSN");
  else if (!allowedHost) setRelayState("bad", "no ingest host");
  else setRelayState("ok", "attached");
}

async function refreshStatus() {
  try {
    renderStatus(await (await fetch("/sentry/status", { cache: "no-store" })).json());
  } catch (err) {
    setRelayState("bad", "device unreachable");
  }
}

/* ---- the relay ---------------------------------------------------------------------- */

async function sendStatusFrame(reqId, result, httpStatus, retryAfterMs) {
  var q = "?req=" + reqId + "&result=" + result +
          "&http=" + (httpStatus || 0) + "&retry=" + (retryAfterMs || 0);
  try {
    await fetch("/sentry/relay/status" + q, { method: "POST" });
  } catch (err) {
    /* The device times out on its own and keeps the event buffered, so a lost status costs
       a retry rather than the report. */
    log("could not report status to device: " + err, "y");
  }
}

/* `Retry-After` is seconds (or an HTTP date); the device wants milliseconds. */
function retryAfterMs(response) {
  var header = response.headers.get("Retry-After");
  if (!header) return 0;
  var seconds = Number(header);
  if (isFinite(seconds)) return Math.max(0, Math.round(seconds * 1000));
  var date = Date.parse(header);
  return isNaN(date) ? 0 : Math.max(0, date - Date.now());
}

async function performRequest(req) {
  var dec = new TextDecoder(), at = 0;
  var url = dec.decode(req.buf.subarray(at, (at += req.urlLen)));
  var auth = dec.decode(req.buf.subarray(at, (at += req.authLen)));
  var ctype = dec.decode(req.buf.subarray(at, (at += req.ctypeLen)));
  var body = req.buf.subarray(at, at + req.bodyLen);

  var parsed;
  try { parsed = new URL(url); }
  catch (err) { await sendStatusFrame(req.reqId, RESULT_REJECTED); return; }

  /* The whitelist. Not defence against this device - the page came from it, so it could
     have shipped any JS it liked. It is defence against a *bug* in the firmware turning
     this tab into an open proxy carrying the user's IP, and it is what the device-side
     RelayTransport enforces independently on its own side. `username` is checked because
     https://ingest.sentry.io@evil.com/ has a host of evil.com but reads like the real
     thing to a human skimming a log. */
  if (parsed.protocol !== "https:" || parsed.host.toLowerCase() !== allowedHost || parsed.username) {
    log("refusing to POST to " + parsed.host + " (allowed: " + allowedHost + ")", "e");
    await sendStatusFrame(req.reqId, RESULT_REJECTED);
    return;
  }

  try {
    var response = await fetch(url, {
      method: "POST",
      headers: { "X-Sentry-Auth": auth, "Content-Type": ctype },
      body: body
    });
    var result = RESULT_ERROR;
    if (response.ok) result = RESULT_OK;
    else if (response.status === 429) result = RESULT_RATE_LIMITED;
    else if (response.status >= 400 && response.status < 500) result = RESULT_REJECTED;

    if (result === RESULT_OK) {
      relayed++;
      document.getElementById("s-sent").textContent = relayed;
      log("relayed " + body.byteLength + " B to Sentry -> " + response.status, "g");
    } else {
      log("relay to Sentry -> " + response.status, "e");
    }
    await sendStatusFrame(req.reqId, result, response.status, retryAfterMs(response));
  } catch (err) {
    /* No network on this side either. UNAVAILABLE tells the device to keep the event
       buffered and try again rather than treating it as delivered. */
    log("relay fetch failed: " + err, "e");
    await sendStatusFrame(req.reqId, RESULT_UNAVAILABLE);
  }
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
    log("device has a " + bodyLen + " B envelope to send");
    pending = { reqId: reqId, urlLen: urlLen, authLen: authLen, ctypeLen: ctypeLen,
                bodyLen: bodyLen, buf: new Uint8Array(total), received: 0, seen: {} };
    return;
  }

  if (!pending || pending.reqId !== reqId) return;

  if (type === FRAME_DATA) {
    if (f.length <= 4) return;
    var offset = dv.getUint16(2, true), n = f.length - 4;
    if (offset + n > pending.buf.length) return;   /* overrun: drop the frame */
    pending.buf.set(f.subarray(4), offset);
    /* Count coverage by offset rather than a running total, so a retransmitted chunk
       cannot make a request with a hole in it look complete. */
    if (!pending.seen[offset]) { pending.seen[offset] = true; pending.received += n; }
    return;
  }

  if (type === FRAME_END) {
    var req = pending;
    pending = null;
    if (req.received < req.buf.length) {
      log("incomplete request (" + req.received + "/" + req.buf.length + " B)", "e");
      sendStatusFrame(reqId, RESULT_ERROR);   /* retryable: the device still has it */
      return;
    }
    performRequest(req);
  }
}

var wasDown = false;

async function poll() {
  try {
    /* The HELLO frame is folded into the poll's query string rather than sent as its own
       round-trip: on this link every poll is already the host announcing itself, and doing
       it this way means a device that rebooted (and forgot us) re-attaches on the very
       next poll with no reconnect logic on either side. */
    var res = await fetch("/sentry/relay/poll?v=" + PROTOCOL_VERSION + "&chunk=" + MAX_CHUNK,
                          { cache: "no-store" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    if (wasDown) { wasDown = false; log("device back", "g"); refreshStatus(); }

    var text = (await res.text()).trim();
    if (!text) return;

    /* The device concatenates its queued frames as [len u16][frame...] repeated, base64 of
       the whole thing. HTTP gives us no packet boundaries, so the link supplies its own —
       exactly the job a BLE notification does for free on the other transport. */
    var bytes = b64ToBytes(text), off = 0;
    while (off + 2 <= bytes.length) {
      var len = bytes[off] | (bytes[off + 1] << 8);
      off += 2;
      if (off + len > bytes.length) break;
      onFrame(bytes.subarray(off, off + len));
      off += len;
    }
  } catch (err) {
    if (!wasDown) {
      wasDown = true;
      pending = null;   /* a half-received request cannot be completed across a reboot */
      setRelayState("warn", "device unreachable - retrying");
      log("device unreachable (rebooting after a crash?)", "y");
    }
  }
}

/* ---- demo buttons -------------------------------------------------------------------- */

document.getElementById("b-test").addEventListener("click", async function () {
  log("asking the device to capture a test message");
  try { log(await (await fetch("/sentry/test")).text()); }
  catch (err) { log("failed: " + err, "e"); }
});

document.getElementById("b-crash").addEventListener("click", async function () {
  if (!confirm("Crash the device? It will panic, write a coredump and reboot.")) return;
  log("asking the device to crash", "y");
  try { log(await (await fetch("/sentry/crash")).text()); }
  catch (err) { log("failed: " + err, "e"); }
});

/* ---- go ------------------------------------------------------------------------------ */

log("relay page loaded");
refreshStatus();
setInterval(poll, POLL_MS);
setInterval(refreshStatus, 5000);
poll();
</script>
</body>
</html>
)PAGE";

#endif /* SENTRY_RELAY_PAGE_H */

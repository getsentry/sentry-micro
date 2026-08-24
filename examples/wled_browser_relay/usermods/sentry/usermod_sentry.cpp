/*
 * sentry usermod — crash reporting for WLED, relayed through the browser
 *
 * The problem this solves: WLED has no TLS anywhere in its tree, and Sentry's ingest is
 * HTTPS-only. Linking mbedTLS into WLED to report a crash is not a byte problem (there is
 * room) so much as an upstream-culture one — every internet-facing feature WLED ships is
 * plaintext, and crash reporting is a poor place to introduce the project's first TLS
 * stack. There is also no WLED backend anywhere to relay through: every wled.me property
 * is static hosting.
 *
 * So this usermod does not give the device a route to Sentry at all. It uses sentry-micro's
 * relay transport (src/transport/sentry_transport_relay.hpp), which chunks a finished HTTP
 * request — URL, two header values, envelope body — over any link that moves short packets
 * both ways, and waits for the host on the other end to report what happened. That link is
 * usually a phone over BLE. Here it is the WLED web UI: `wled_ui_relay.js` (alongside this
 * file, appended to WLED's own index.js) polls the routes below and performs the POST. The
 * browser already has a TLS stack, a CA store and an internet connection, and this borrows
 * all three for the length of one request.
 *
 * There is deliberately **no page to open**. The relay rides the UI session the user
 * already has — open WLED the way you always would and the reports go out. A device nobody
 * has looked at in a week keeps its reports in flash until somebody does.
 *
 * What that buys, concretely:
 *   - no TLS, no certificates, no CA bundle, and no mbedTLS in the image
 *   - nothing to install, run or operate: no forwarder process, no broker, no WLED backend
 *   - the DSN is the project's real DSN, not a rewritten one pointing at a local relay
 *
 * What it costs: reports only leave the device while somebody has WLED open in a browser.
 * That is the correct trade for a crash reporter — the crash is discovered on the *next*
 * boot from the coredump partition, written to a flash-backed queue, and delivered whenever
 * a relay next attaches. Compare examples/wled_http_forwarder in this repo, which trades
 * that requirement for a small script the developer runs on their own LAN.
 *
 * ROUTES ADDED
 *   GET  /sentry/status             what the SDK knows: DSN host, release, queue depth
 *   GET  /sentry/relay/poll         queued device -> host frames, base64
 *   POST /sentry/relay/status       the host's verdict on one relayed request
 *   GET  /sentry/test               capture a message event, to prove the path works
 *   GET  /sentry/crash              panic on purpose, 500ms from now
 *
 * The last two are demo affordances with no UI attached — `curl http://<device>/sentry/test`
 * or just visit the URL. Everything else is machine-to-machine.
 *
 * WLED's v2 usermods self-register via REGISTER_USERMOD (a linker-section trick, see
 * wled00/fcn_declare.h), so no WLED *source* file is touched by the usermod itself. The one
 * change inside the WLED checkout is appending wled_ui_relay.js to wled00/data/index.js —
 * see this example's README, and see that file's own header for why the host side has to
 * live in the UI rather than on a page of its own.
 *
 * Coredump support depends on which Arduino-ESP32 core this links against — see
 * platformio_override.ini's `platform =` line before assuming a real backtrace will show up.
 * The `[sentry] coredump: supported=.. available=..` line printed at boot is the fastest way
 * to tell which state a given build is in.
 */

#include "wled.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

#include <core/sentry_base64.h>
#include <core/sentry_relay.h>
#include <device/sentry_coredump_device.h>
#include <device/sentry_storage_nvs.h>
#include <sentry_micro.h>
#include <transport/sentry_transport_relay.hpp>

/*
 * Your project's real DSN — unlike this repo's forwarder example, nothing here rewrites it.
 * The browser performs the request, so the host in this DSN is the host the browser
 * connects to, and the device never needs a route to it.
 *
 * Set at build time from the environment (SENTRY_MICRO_DSN, via scripts/env_secrets.py);
 * the placeholder below only exists so a bare `pio run` still compiles. A DSN in a released
 * binary is extractable with `strings`, so for anything beyond a demo, provision it at
 * runtime instead — WLED's own settings pages are the natural place.
 */
#ifndef SENTRY_DSN
#    define SENTRY_DSN "https://examplePublicKey@o0.ingest.sentry.io/0"
#endif

#ifndef SENTRY_MICRO_DEBUG
#    define SENTRY_MICRO_DEBUG 1
#endif

#define SENTRY_UM_STRINGIFY_(x) #x
#define SENTRY_UM_STRINGIFY(x) SENTRY_UM_STRINGIFY_(x)

/*
 * scripts/release.sh sets SENTRY_MICRO_RELEASE (-> this macro, via env_secrets.py) to
 * whatever it registers with Sentry and stamped the matching debug file under — using
 * anything else here would report events under a release Sentry never heard of. Falls back
 * to WLED's own version for a plain `pio run` with no release.sh involved; that build still
 * works, it just has no debug file to symbolicate against.
 */
#ifndef SENTRY_RELEASE
#    define SENTRY_RELEASE "wled@" SENTRY_UM_STRINGIFY(VERSION)
#endif

namespace {

/*
 * How much of one request the device may put in a single DATA frame.
 *
 * On a BLE link this is the negotiated MTU and the SDK's 180-byte default is right. Over
 * HTTP there is no MTU to respect — a bigger chunk just means fewer frames in the same
 * response — so this is raised to match what the relay page announces. The transport clamps
 * to whichever of the two is smaller, so the two constants disagreeing is safe, not a bug.
 */
constexpr size_t kRelayChunkBytes = 1024;

/*
 * Room for one request's worth of frames waiting to be collected.
 *
 * One request is BEGIN + ceil(payload / kRelayChunkBytes) DATA + END, where the payload is
 * url + auth + content-type + a 2 KB envelope: about 2.3 KB of frames, plus this queue's own
 * 2-byte length prefix per frame. 4 KB leaves headroom without being worth a malloc. If a
 * request ever does overflow it, the enqueue fails, `send()` reports UNAVAILABLE, and the
 * envelope stays in the offline buffer for the next attempt — the same path a disconnected
 * relay takes.
 */
constexpr size_t kRelayQueueBytes = 4096;

/*
 * How long after the last poll the host counts as gone.
 *
 * Generous relative to the page's 1s poll: closing the tab should detach, but a slow page
 * load or a browser throttling a background tab should not, because detaching mid-request
 * voids a STATUS the device is still waiting for.
 */
constexpr uint32_t kHostIdleTimeoutMs = 8000;

/*
 * A deliberate abort() rather than a null-pointer write: whether the latter actually faults
 * depends on region-protection configuration that varies by platform pin, while abort()
 * triggers ESP-IDF's panic handler unconditionally and produces a coredump the same way any
 * other fatal panic does.
 *
 * Still three calls deep and noinline, so the backtrace has multiple frames to symbolicate —
 * a demo whose stack is one frame deep proves very little.
 */
void __attribute__((noinline)) sentryDemoCrashInnermost() { abort(); }
void __attribute__((noinline)) sentryDemoCrashMiddle() { sentryDemoCrashInnermost(); }
void __attribute__((noinline)) sentryDemoCrashOuter() { sentryDemoCrashMiddle(); }

} // namespace

class UsermodSentry : public Usermod {
public:
    void setup() override
    {
        queueLock_ = xSemaphoreCreateMutex();

        sentry::Options options;
        options.dsn = SENTRY_DSN;
        options.release = SENTRY_RELEASE;
        options.environment = "development";
        options.board = ARDUINO_BOARD;
        options.debug = SENTRY_MICRO_DEBUG;

        if (!sentry::init(options)) {
            /* A bad DSN disables the SDK, not the firmware — crash reporting must never be
             * load-bearing for a lighting controller. Every route below still registers, so
             * /sentry/status can say *why* nothing is being reported. */
            DEBUG_PRINTLN(F("[sentry] init failed — check SENTRY_DSN"));
        }

        /* Sixteen slots of NVS. This is what makes the whole design work: a crash is
         * recovered in setup(), long before any browser has the relay page open, so the
         * report has to survive in flash until one does — and across further reboots. */
        if (!sentry_enable_buffering(sentry_storage_nvs(16))) {
            DEBUG_PRINTLN(F("[sentry] NVS unavailable — running without an offline buffer"));
        }

        relay_.set_chunk_bytes(kRelayChunkBytes);
        sentry::set_transport(relay_);

        registerRoutes();

        /* Delivery runs on its own task, not in loop(). RelayTransport::send() blocks until
         * the browser answers or the timeout expires — a whole HTTP round-trip through a
         * page polling once a second. Doing that from loop() would stall WLED's render for
         * as long as it takes, which for a lighting controller is the one cost that would
         * actually disqualify this. Priority 1 (below Arduino's loop task at 2) and pinned
         * to core 0, so it cannot compete with rendering even while blocked.
         *
         * Everything that touches the SDK after setup() runs on this task; the HTTP handlers
         * below only touch the frame queue (under queueLock_) and RelayTransport's own
         * cross-task entry points, which document themselves as safe to call from another
         * task. */
        xTaskCreatePinnedToCore(deliveryTask, "sentry", 8192, this, 1, nullptr, 0);

        /* A recovered crash is the more interesting event; reporting both on the same boot
         * would double up. */
        DEBUG_PRINTF("[sentry] coredump: supported=%d available=%d\n",
            (int)sentry_coredump_is_supported(), (int)sentry_coredump_available());
        if (!reportCrash()) {
            reportBoot();
        }
    }

    /**
     * WLED calls this once the station has an address — which setup() does not have yet, so
     * this is the first moment the hint below can name a URL somebody can actually open.
     */
    void connected() override
    {
        DEBUG_PRINTF("[sentry] open http://%s/ in a browser to relay %u queued event(s)\n",
            WiFi.localIP().toString().c_str(), (unsigned)sentry_buffered_count());
    }

    void loop() override
    {
        /* The only thing left on the render loop: the crash trigger, which cannot run from
         * the HTTP handler that requests it (the response would never flush) and must not
         * run from the delivery task (a panic there would be a stack nobody recognises). */
        if (crashPending_ && millis() >= crashAtMs_) {
            crashPending_ = false;
            sentryDemoCrashOuter();
            /* Should never print — abort() panics unconditionally. If it does, this
             * toolchain's abort()-triggers-panic assumption no longer holds and the crash
             * trigger needs re-verifying. */
            DEBUG_PRINTLN(F("[sentry] crash call RETURNED — no fault occurred"));
        }
    }

    /**
     * Two lines on WLED's Info page, so somebody who never read this README still finds out
     * the usermod is there and whether anything is waiting to be sent.
     */
    void addToJsonInfo(JsonObject &root) override
    {
        JsonObject user = root["u"];
        if (user.isNull()) {
            user = root.createNestedObject("u");
        }

        JsonArray relayInfo = user.createNestedArray(F("Sentry relay"));
        relayInfo.add(relay_.host_ready() ? F("attached") : F("no browser attached"));

        JsonArray queueInfo = user.createNestedArray(F("Sentry queue"));
        queueInfo.add(sentry_buffered_count());
        queueInfo.add(F(" events"));
    }

private:
    /* ---- the relay link ------------------------------------------------------------- */

    /**
     * Put one frame on the wire — except that on this link there is no wire to put it on
     * until the browser asks. Frames are appended to a queue that `/sentry/relay/poll`
     * drains, each behind a 2-byte length, because HTTP hands the other end a byte stream
     * with no packet boundaries in it. A BLE notification supplies those boundaries for
     * free; an HTTP body has to carry its own.
     *
     * Returning false (queue full) aborts the request with UNAVAILABLE, which leaves the
     * envelope in the offline buffer for a later attempt. That is the correct failure: the
     * alternative — dropping frames and sending an incomplete request — would have the
     * browser POST a truncated envelope that ingest rejects for good.
     */
    static bool relayWrite(void *ctx, const uint8_t *frame, size_t len)
    {
        return static_cast<UsermodSentry *>(ctx)->enqueueFrame(frame, len);
    }

    bool enqueueFrame(const uint8_t *frame, size_t len)
    {
        if (!queueLock_ || len > 0xffff) {
            return false;
        }
        bool queued = false;
        if (xSemaphoreTake(queueLock_, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (queueLen_ + 2 + len <= sizeof(queue_)) {
                queue_[queueLen_++] = (uint8_t)(len & 0xff);
                queue_[queueLen_++] = (uint8_t)(len >> 8);
                memcpy(queue_ + queueLen_, frame, len);
                queueLen_ += len;
                queued = true;
            }
            xSemaphoreGive(queueLock_);
        }
        return queued;
    }

    /* ---- HTTP ------------------------------------------------------------------------ */

    /*
     * A note on URIs, because the trap here is silent: this AsyncWebServer matches a
     * handler's URI as a *prefix*, not an exact path — AsyncCallbackWebHandler::canHandle()
     * accepts when `_uri == url` **or** `url.startsWith(_uri + "/")` — and dispatches to the
     * first handler that accepts. No route below is a prefix of another, so order does not
     * matter today. Add one that is (a bare "/sentry", say) and it will swallow every route
     * under it, answering all of them with its own content, and every caller will fail while
     * parsing rather than at the request.
     */
    void registerRoutes()
    {
        server.on("/sentry/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->send(200, "application/json", statusJson());
        });

        server.on("/sentry/relay/poll", HTTP_GET,
            [this](AsyncWebServerRequest *request) { handlePoll(request); });

        server.on("/sentry/relay/status", HTTP_POST,
            [this](AsyncWebServerRequest *request) { handleRelayStatus(request); });

        server.on("/sentry/test", HTTP_GET, [this](AsyncWebServerRequest *request) {
            /* Queued for the delivery task rather than captured here: capture_message()
             * calls the transport, and the transport blocks waiting for a browser that
             * cannot answer while this very handler is occupying the async_tcp task. */
            testPending_ = true;
            request->send(200, "text/plain", "Test event queued.\n");
        });

        server.on("/sentry/crash", HTTP_GET, [this](AsyncWebServerRequest *request) {
            request->send(200, "text/plain",
                "Crashing in 500ms. The report is built on the next boot and relayed by "
                "whichever browser next has WLED open.\n");
            /* Deadline set before the pending flag, not after: loop() runs on the other core
             * and only reads crashAtMs_ once it observes crashPending_ true, so this order
             * guarantees it never reads a stale (default-zero) deadline and fires
             * immediately instead of after the 500ms the response needs to flush. */
            crashAtMs_ = millis() + 500;
            crashPending_ = true;
        });
    }

    String statusJson()
    {
        const sentry_dsn_t &dsn = sentry::dsn();
        String out = F("{\"enabled\":");
        out += sentry::is_enabled() ? F("true") : F("false");
        /* The host the page is allowed to POST to. Sent rather than compiled into the page
         * so that changing the DSN needs no change here — and it is the same string
         * RelayTransport checks the URL against on this side, so the two ends cannot
         * disagree about what "the ingest host" means. */
        out += F(",\"host\":\"");
        out += dsn.valid ? dsn.host : "";
        out += F("\",\"project\":\"");
        out += dsn.valid ? dsn.project_id : "";
        out += F("\",\"release\":\"");
        out += SENTRY_RELEASE;
        out += F("\",\"attached\":");
        out += relay_.host_ready() ? F("true") : F("false");
        out += F(",\"buffered\":");
        out += sentry_buffered_count();
        out += F(",\"dropped\":");
        out += sentry_dropped_count();
        out += F(",\"coredumpSupported\":");
        out += sentry_coredump_is_supported() ? F("true") : F("false");
        out += F(",\"coredumpAvailable\":");
        out += sentry_coredump_available() ? F("true") : F("false");
        out += F(",\"uptime\":");
        out += millis() / 1000;
        out += F("}");
        return out;
    }

    void handlePoll(AsyncWebServerRequest *request)
    {
        lastPollMs_ = millis();

        /*
         * HELLO, folded into the poll's query string.
         *
         * On a BLE link the host announces itself once, on connect. HTTP has no connect —
         * every poll is already the host saying "I am here, I speak version N, my frames may
         * be this big", so spending a separate round-trip to say it again would be ceremony.
         * More usefully, it makes reattachment free: a device that panicked and rebooted has
         * forgotten this host, and the very next poll re-announces it with no reconnect logic
         * on either side. That case is not an edge — it is what every crash does.
         */
        if (!relay_.host_ready()) {
            uint16_t chunk = kRelayChunkBytes;
            uint8_t version = SENTRY_RELAY_PROTOCOL_VERSION;
            if (request->hasArg("chunk")) {
                chunk = (uint16_t)request->arg("chunk").toInt();
            }
            if (request->hasArg("v")) {
                version = (uint8_t)request->arg("v").toInt();
            }
            const uint8_t hello[SENTRY_RELAY_HELLO_LEN] = { SENTRY_RELAY_FRAME_HELLO, version,
                (uint8_t)(chunk & 0xff), (uint8_t)(chunk >> 8) };
            /* Attached first: on_host_frame() records the HELLO, and host_ready() is the
             * conjunction of the two, so the reverse order would leave a window where the
             * hello is recorded against a host still marked absent. */
            relay_.set_host_attached(true);
            relay_.on_host_frame(hello, sizeof(hello));
            DEBUG_PRINTF("[sentry] relay host attached (v%u, chunk %u)\n", (unsigned)version,
                (unsigned)chunk);
        }

        /*
         * Base64 rather than an octet-stream body, for one unglamorous reason: the response
         * types this AsyncWebServer offers are a String (not binary-safe — an envelope is
         * full of bytes a String will not carry) or a pointer it holds onto and reads later
         * (not safe either — the queue it would point into is refilled by the next send).
         * A third more bytes buys a body that is correct with neither caveat, and one that
         * `curl http://<device>/sentry/relay/poll` prints legibly while debugging.
         */
        String body;
        if (queueLock_ && xSemaphoreTake(queueLock_, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (queueLen_ > 0) {
                size_t need = SENTRY_BASE64_ENCODED_LEN(queueLen_) + 1;
                /* Heap, and only on the polls that actually carry frames — which is a small
                 * minority of them. A permanent scratch buffer would be ~5.5 KB of BSS on a
                 * chip where a usermod's whole budget is measured in kilobytes, spent almost
                 * entirely on being empty. Encoding on the async_tcp task's stack is not an
                 * option either; it does not have 5.5 KB to spare. */
                char *encoded = new (std::nothrow) char[need];
                if (encoded) {
                    size_t written = sentry_base64_encode(encoded, need, queue_, queueLen_);
                    if (written > 0 && written < need) {
                        body = encoded;
                        /* Cleared only once the bytes are safely in `body`: a failed encode
                         * or a failed copy leaves the queue intact for the next poll, which
                         * costs a second and loses nothing. */
                        queueLen_ = 0;
                    }
                    delete[] encoded;
                }
            }
            xSemaphoreGive(queueLock_);
        }
        request->send(200, "text/plain", body);
    }

    void handleRelayStatus(AsyncWebServerRequest *request)
    {
        if (!request->hasArg("req")) {
            request->send(400, "text/plain", "missing req\n");
            return;
        }
        /* Rebuilt into a real STATUS frame rather than poked into the transport directly:
         * on_host_frame() is the documented cross-task entry point, and going through it
         * means this link's frames are parsed by exactly the same code as a BLE host's. */
        uint32_t retry = request->hasArg("retry") ? (uint32_t)request->arg("retry").toInt() : 0;
        uint16_t http = request->hasArg("http") ? (uint16_t)request->arg("http").toInt() : 0;
        uint8_t result = request->hasArg("result") ? (uint8_t)request->arg("result").toInt()
                                                   : (uint8_t)SENTRY_RELAY_RESULT_ERROR;
        const uint8_t status[SENTRY_RELAY_STATUS_LEN] = { SENTRY_RELAY_FRAME_STATUS,
            (uint8_t)request->arg("req").toInt(), result, (uint8_t)(http & 0xff),
            (uint8_t)(http >> 8), (uint8_t)(retry & 0xff), (uint8_t)((retry >> 8) & 0xff),
            (uint8_t)((retry >> 16) & 0xff), (uint8_t)((retry >> 24) & 0xff) };
        relay_.on_host_frame(status, sizeof(status));
        request->send(200, "text/plain", "ok\n");
    }

    /* ---- delivery -------------------------------------------------------------------- */

    static void deliveryTask(void *arg)
    {
        UsermodSentry *self = static_cast<UsermodSentry *>(arg);
        for (;;) {
            self->deliveryTick();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    void deliveryTick()
    {
        /* A browser that closed the tab stops polling and never says goodbye, so absence is
         * the only signal there is. Marking it detached matters: without it the transport
         * would keep believing a host is there and block a full timeout per envelope. */
        if (relay_.host_ready() && millis() - lastPollMs_ > kHostIdleTimeoutMs) {
            relay_.set_host_attached(false);
            DEBUG_PRINTLN(F("[sentry] relay host went away"));
        }

        if (!relay_.host_ready()) {
            return;
        }

        if (testPending_) {
            testPending_ = false;
            sentry_response_t response
                = sentry::capture_message(SENTRY_LEVEL_INFO, "WLED test event from the relay page");
            DEBUG_PRINTF("[sentry] test event result %d (http %u)\n", (int)response.result,
                (unsigned)response.http_status);
        }

        if (sentry_buffered_count() > 0) {
            sentry_flush(2);
        }
    }

    /* ---- reporting -------------------------------------------------------------------- */

    /**
     * Report a crash recovered from the coredump partition, if there is one.
     * Mirrors examples/wifi_basic's report_crash() — see that file for the envelope-building
     * rationale. The one difference here is where it lands: no relay is attached this early,
     * so this reliably goes to the offline buffer rather than to Sentry, and the delivery
     * task ships it once a browser shows up.
     */
    bool reportCrash()
    {
        char eventId[SENTRY_MICRO_EVENT_ID_LEN];
        sentry_event_t event;
        sentry_coredump_t crash;

        if (!sentry_event_prepare(&event, eventId)) {
            return false;
        }
        if (!sentry_event_attach_coredump(&event, &crash)) {
            return false;
        }

        char message[128];
        snprintf(message, sizeof(message), "%s in task %s",
            crash.exception_type[0] ? crash.exception_type : "Panic", crash.task_name);
        event.message = message;

        DEBUG_PRINTLN(F("[sentry] crash recovered from the previous boot"));
        DEBUG_PRINTF("[sentry] exception %s at pc 0x%08x, %u frame(s)%s\n", crash.exception_type,
            (unsigned)crash.exception_pc, (unsigned)crash.frame_count,
            crash.truncated ? " (truncated)" : "");

        char envelope[SENTRY_MICRO_ENVELOPE_BUFFER_BYTES];
        size_t needed = sentry_envelope_write(envelope, sizeof(envelope), &event);
        if (needed == 0 || needed >= sizeof(envelope)) {
            DEBUG_PRINTF("[sentry] crash envelope needs %u bytes\n", (unsigned)needed);
            return true;
        }

        uint32_t bufferedBefore = sentry_buffered_count();
        sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);
        DEBUG_PRINTF("[sentry] crash report result %d (http %u)\n", (int)response.result,
            (unsigned)response.http_status);

        /* Erase only once *this* envelope is somewhere durable — delivered, or newly
         * buffered. sentry_buffered_count() > 0 alone cannot tell that apart from a buffer
         * that already held older entries: if this envelope fails to send and also fails to
         * enqueue (buffer full, storage error), that check still passes on the old entries
         * alone, and erasing here would lose the crash just recovered without it ever
         * landing anywhere. Comparing against the count taken before the send is the only
         * way to distinguish the two without sentry_send_envelope() reporting it. */
        bool durablyHeld
            = response.result == SENTRY_SEND_OK || sentry_buffered_count() > bufferedBefore;
        if (durablyHeld) {
            sentry_coredump_erase();
            DEBUG_PRINTLN(F("[sentry] coredump erased"));
        } else {
            DEBUG_PRINTLN(F("[sentry] coredump kept for the next boot"));
        }
        return true;
    }

    void reportBoot()
    {
        const sentry_device_info_t &dev = sentry::device_info();
        bool crashed = sentry_reset_reason_is_crash(dev.reset_reason);

        char message[96];
        snprintf(message, sizeof(message), "WLED booted: %s",
            sentry_reset_reason_name(dev.reset_reason));

        sentry_response_t response
            = sentry::capture_message(crashed ? SENTRY_LEVEL_FATAL : SENTRY_LEVEL_INFO, message);
        DEBUG_PRINTF("[sentry] boot report result %d (http %u)\n", (int)response.result,
            (unsigned)response.http_status);
    }

    /* The SDK stores a pointer to this, so it has to outlive registration — see
     * sentry::set_transport()'s contract. The usermod instance is a static global (below),
     * so a member is fine. */
    sentry::RelayTransport relay_ { &UsermodSentry::relayWrite, this };

    SemaphoreHandle_t queueLock_ = nullptr;
    uint8_t queue_[kRelayQueueBytes];
    size_t queueLen_ = 0;

    /* atomic, not plain: written from AsyncTCP's own FreeRTOS task (which services the
     * server.on() handlers) and read from loop() or the delivery task — on a dual-core ESP32
     * those are different cores, and a plain write has no guaranteed visibility or ordering
     * across that boundary. Same reasoning as RelayTransport's own status_pending_ pair
     * (src/transport/sentry_transport_relay.hpp). */
    std::atomic<bool> crashPending_ { false };
    std::atomic<uint32_t> crashAtMs_ { 0 };
    std::atomic<bool> testPending_ { false };
    std::atomic<uint32_t> lastPollMs_ { 0 };
};

static UsermodSentry sentryUsermod;
REGISTER_USERMOD(sentryUsermod);

/**
 * Client-side throttle for captured messages.
 *
 * `sentry_capture_message()` is easy to call from a loop, and a loop is where firmware
 * lives. A sensor that starts failing at 50 Hz does not produce one issue in Sentry — it
 * produces 50 events a second, indefinitely, against a quota the user is paying for, from
 * a device nobody is looking at. On a desktop SDK that is annoying; here it is the
 * difference between a diagnostic and a denial of service you inflicted on yourself.
 *
 * Distinct from the backoff in `sentry_micro.c`, which honours a `Retry-After` the *server*
 * asked for. This one never talks to anyone: it decides, locally and before any bytes are
 * built, whether this particular message is worth an event at all.
 *
 * Two rules, in this order:
 *
 *   1. **Repeats.** The same message at the same level, within `repeat_window_ms`, is
 *      suppressed. This is the failing-sensor case exactly, and it is checked first so a
 *      repeat does not consume the budget a *different* message could have used.
 *   2. **Volume.** At most `max_per_minute` messages get through in any 60-second window,
 *      whatever they say. The backstop for code that generates unbounded distinct
 *      messages — an error string with a counter in it, say, which rule 1 cannot catch.
 *
 * In `core/` and pure: no clock of its own, no allocation, no device calls. `now_ms` is
 * passed in, which is what lets the host tests drive a year of traffic through it in
 * milliseconds instead of inferring behaviour from a board.
 */
#ifndef SENTRY_MICRO_THROTTLE_H_INCLUDED
#define SENTRY_MICRO_THROTTLE_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_envelope.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How long a volume window lasts. Fixed rather than configurable; the budget is the knob. */
#define SENTRY_MICRO_THROTTLE_WINDOW_MS 60000u

typedef struct {
    /** Messages allowed per 60s window. 0 means unlimited. */
    uint16_t max_per_minute;
    /** How long an identical message stays suppressed, in ms. 0 disables the rule. */
    uint32_t repeat_window_ms;

    /* State below; do not touch. Zeroed by sentry_throttle_init(). */
    uint64_t window_start_ms;
    uint16_t sent_in_window;
    uint32_t last_digest;
    uint64_t last_digest_ms;
    bool has_last_digest;
    uint32_t suppressed;
} sentry_throttle_t;

/** Configure and reset. Both limits may be 0, which turns that rule off. */
void sentry_throttle_init(
    sentry_throttle_t *throttle, uint16_t max_per_minute, uint32_t repeat_window_ms);

/**
 * Decide whether this message should become an event, and record the decision.
 *
 * Call exactly once per capture: it is not a query, it spends the budget. Returns false
 * when the message should be dropped, in which case `sentry_throttle_suppressed()` has
 * gone up by one.
 *
 * A NULL message is treated as the empty string rather than rejected — dropping an event
 * because its text was missing would lose the level, timestamp and device context that are
 * still perfectly good.
 */
bool sentry_throttle_allow(
    sentry_throttle_t *throttle, sentry_level_t level, const char *message, uint64_t now_ms);

/**
 * How many captures have been dropped since init.
 *
 * Worth surfacing rather than keeping private: a throttle that silently eats events looks
 * exactly like a device that has stopped having problems.
 */
uint32_t sentry_throttle_suppressed(const sentry_throttle_t *throttle);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_THROTTLE_H_INCLUDED */

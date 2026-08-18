#include "sentry_throttle.h"

#include <string.h>

/**
 * FNV-1a over the level and the message text.
 *
 * A 32-bit digest rather than keeping the string: the throttle must not own memory, and a
 * pointer would not survive the caller's `snprintf` buffer going out of scope. The cost is
 * that two different messages could collide and one of them be suppressed for a window —
 * at roughly one chance in four billion per pair, against a rule whose whole purpose is
 * dropping events, that is a trade worth making.
 */
static uint32_t digest_of(sentry_level_t level, const char *message)
{
    uint32_t hash = 2166136261u;
    hash = (hash ^ (uint32_t)level) * 16777619u;
    for (const char *c = message; *c; c++) {
        hash = (hash ^ (uint8_t)*c) * 16777619u;
    }
    return hash;
}

void sentry_throttle_init(
    sentry_throttle_t *throttle, uint16_t max_per_minute, uint32_t repeat_window_ms)
{
    if (!throttle) {
        return;
    }
    memset(throttle, 0, sizeof(*throttle));
    throttle->max_per_minute = max_per_minute;
    throttle->repeat_window_ms = repeat_window_ms;
}

bool sentry_throttle_allow(
    sentry_throttle_t *throttle, sentry_level_t level, const char *message, uint64_t now_ms)
{
    if (!throttle) {
        return true;
    }

    /* Rule 1: the same message, again, too soon.
     *
     * `last_digest_ms` advances when a message is allowed and again when it settles, but
     * never on a suppressed repeat. Sliding it forward on every suppressed call would mean
     * a loop calling faster than the window never sends a second event at all — the failure
     * would go quiet rather than being reported once per window, which is the opposite of
     * what this is for. */
    const char *text = message ? message : "";
    uint32_t digest = digest_of(level, text);
    if (throttle->repeat_window_ms > 0 && throttle->has_last_digest
        && digest == throttle->last_digest
        && now_ms - throttle->last_digest_ms < throttle->repeat_window_ms) {
        throttle->suppressed++;
        return false;
    }

    /* Rule 2: too many messages, whatever they say.
     *
     * A fixed window rather than a sliding one or a token bucket. It admits a burst across
     * a window boundary — up to 2x the budget in a moment — and in exchange it is two
     * integers of state and a rule anyone can predict from the outside. For a quota
     * backstop that is the right end of the trade; the precise instant an event was
     * dropped is not something anybody will reason about.
     *
     * A first call at now_ms == 0 starts a window like any other: window_start_ms is 0 and
     * sent_in_window is 0, so the elapsed check below is false and the window is simply
     * the one already in progress. */
    if (throttle->max_per_minute > 0) {
        if (now_ms - throttle->window_start_ms >= SENTRY_MICRO_THROTTLE_WINDOW_MS) {
            throttle->window_start_ms = now_ms;
            throttle->sent_in_window = 0;
        }
        if (throttle->sent_in_window >= throttle->max_per_minute) {
            throttle->suppressed++;
            return false;
        }
        throttle->sent_in_window++;
    }

    throttle->last_digest = digest;
    throttle->last_digest_ms = now_ms;
    throttle->has_last_digest = true;
    return true;
}

void sentry_throttle_settle(sentry_throttle_t *throttle, uint64_t now_ms)
{
    /* Only meaningful for a message that was allowed; there is nothing else it could be
     * describing, and moving the mark for a suppressed one would extend a window the
     * caller never spent any time on. */
    if (throttle && throttle->has_last_digest) {
        throttle->last_digest_ms = now_ms;
    }
}

uint32_t sentry_throttle_suppressed(const sentry_throttle_t *throttle)
{
    return throttle ? throttle->suppressed : 0;
}

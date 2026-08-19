#include "sentry_span.h"

#include <string.h>

void sentry_transaction_begin(sentry_transaction_t *txn, const sentry_trace_context_t *trace,
    const char *name, const char *op, uint64_t now_uptime_us)
{
    if (!txn) {
        return;
    }
    memset(txn, 0, sizeof(*txn));
    if (trace) {
        txn->trace = *trace;
    }
    txn->name = name;
    txn->op = op;
    txn->start_uptime_us = now_uptime_us;
    txn->active = true;
}

sentry_span_t *sentry_span_open(sentry_transaction_t *txn, const char *op, const char *description,
    const uint8_t span_id_bytes[8], uint64_t now_uptime_us)
{
    if (!txn || !txn->active || !span_id_bytes) {
        return NULL;
    }
    if (txn->span_count >= SENTRY_MICRO_MAX_SPANS) {
        /* Counted rather than ignored. A trace missing spans nobody knows about reads as a
         * complete picture of a simpler operation than actually ran. */
        txn->dropped_spans++;
        return NULL;
    }

    sentry_span_t *span = &txn->spans[txn->span_count++];
    memset(span, 0, sizeof(*span));
    span->op = op;
    span->description = description;
    span->start_uptime_us = now_uptime_us;
    sentry_span_id_format(span->span_id, span_id_bytes);
    return span;
}

void sentry_span_close(sentry_span_t *span, uint64_t now_uptime_us)
{
    /* First close wins. A second one is a bug in the caller, and moving the end time would
     * quietly stretch the span to cover work it did not do. */
    if (span && !span->finished) {
        span->end_uptime_us = now_uptime_us;
        span->finished = true;
    }
}

void sentry_span_set_number(sentry_span_t *span, const char *key, int64_t value)
{
    if (!span || !key) {
        return;
    }
    for (uint8_t i = 0; i < span->attr_count; i++) {
        if (strcmp(span->attrs[i].key, key) == 0) {
            span->attrs[i].value = value;
            return;
        }
    }
    if (span->attr_count < SENTRY_MICRO_MAX_SPAN_ATTRS) {
        span->attrs[span->attr_count].key = key;
        span->attrs[span->attr_count].value = value;
        span->attr_count++;
    }
}

bool sentry_transaction_end_at(
    sentry_transaction_t *txn, uint64_t now_uptime_us, uint64_t now_unix_us)
{
    if (!txn || !txn->active) {
        return false;
    }
    txn->end_uptime_us = now_uptime_us;
    txn->active = false;

    /* Any span the caller forgot ends with the transaction rather than staying open, which
     * would otherwise be written as a zero-length span at the start. */
    for (uint8_t i = 0; i < txn->span_count; i++) {
        sentry_span_close(&txn->spans[i], now_uptime_us);
    }

    if (now_unix_us == 0) {
        return false;
    }
    txn->end_unix_us = now_unix_us;
    return true;
}

uint64_t sentry_transaction_start_unix_us(const sentry_transaction_t *txn)
{
    if (!txn || txn->end_unix_us == 0) {
        return 0;
    }
    /* The duration is monotonic and therefore right even if the clock was set part way
     * through the operation; only the anchor comes from the wall clock. */
    uint64_t elapsed = txn->end_uptime_us - txn->start_uptime_us;
    return elapsed > txn->end_unix_us ? 0 : txn->end_unix_us - elapsed;
}

/**
 * Common definitions shared by every sentry-micro translation unit.
 *
 * Mirrors the role of sentry-native's `sentry_boot.h`: the one header that every
 * internal file includes first, defining the version, the export macros, and the
 * size limits. Everything here is freestanding C — no Arduino, no ESP-IDF — so the
 * portable core can also be compiled and unit-tested on a host machine.
 */
#ifndef SENTRY_MICRO_BOOT_H_INCLUDED
#define SENTRY_MICRO_BOOT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENTRY_MICRO_SDK_NAME "sentry.micro.esp32"
#define SENTRY_MICRO_SDK_VERSION "0.1.0"
#define SENTRY_MICRO_SDK_USER_AGENT SENTRY_MICRO_SDK_NAME "/" SENTRY_MICRO_SDK_VERSION

/*
 * Size limits.
 *
 * Everything in the core is fixed-size and stack- or caller-allocated: an MCU crash
 * handler cannot rely on the heap (it may be exhausted, or fragmented, or the very
 * thing that crashed), so the SDK never mallocs on the reporting path. These bounds
 * are the price of that guarantee — pick them generously enough for real DSNs but
 * small enough that a parsed DSN still fits comfortably on a FreeRTOS task stack.
 */
#define SENTRY_MICRO_MAX_DSN_LEN 256
#define SENTRY_MICRO_MAX_HOST_LEN 128
#define SENTRY_MICRO_MAX_KEY_LEN 64
#define SENTRY_MICRO_MAX_PATH_LEN 64
#define SENTRY_MICRO_MAX_PROJECT_ID_LEN 24
/* Ingest URL + the `X-Sentry-Auth` header value, both built once at init. */
#define SENTRY_MICRO_MAX_URL_LEN 320
#define SENTRY_MICRO_MAX_AUTH_LEN 256

#endif /* SENTRY_MICRO_BOOT_H_INCLUDED */

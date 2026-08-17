/*
 * Copy this file to `secrets.h` next to it and fill in your own values:
 *
 *     cp src/secrets.example.h src/secrets.h
 *
 * `secrets.h` is gitignored. main.cpp picks it up automatically via __has_include, so the
 * example still builds without it (with placeholders) — it just will not connect.
 *
 * Prefer environment variables if you would rather not have credentials on disk at all;
 * they take precedence over this file and work in CI, where it does not exist:
 *
 *     export SENTRY_MICRO_WIFI_SSID='My Network'
 *     export SENTRY_MICRO_WIFI_PASSWORD='hunter2'
 *     export SENTRY_MICRO_DSN='https://key@o0.ingest.sentry.io/1'
 *
 * The #ifndef guards are what make that precedence work — without them this file would
 * redefine the macros the environment already set. Keep them.
 */
#pragma once

#ifndef WIFI_SSID
#    define WIFI_SSID "your-wifi-ssid"
#endif

#ifndef WIFI_PASSWORD
#    define WIFI_PASSWORD "your-wifi-password"
#endif

/* From Sentry: Settings → Projects → <project> → Client Keys (DSN). */
#ifndef SENTRY_DSN
#    define SENTRY_DSN "https://examplePublicKey@o0.ingest.sentry.io/0"
#endif

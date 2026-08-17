/*
 * Copy this file to `secrets.h` next to it and fill in your own values:
 *
 *     cp src/secrets.example.h src/secrets.h
 *
 * `secrets.h` is gitignored. main.cpp picks it up automatically via __has_include, so the
 * example still builds without it (with the placeholders below) — it just will not connect.
 *
 * CI and scripted builds can skip the file entirely and pass the same names as build flags:
 *
 *     pio run -e esp32dev --build-flag='-D WIFI_SSID=\"...\"'
 */
#pragma once

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASSWORD "your-wifi-password"

/* From Sentry: Settings → Projects → <project> → Client Keys (DSN). */
#define SENTRY_DSN "https://examplePublicKey@o0.ingest.sentry.io/0"

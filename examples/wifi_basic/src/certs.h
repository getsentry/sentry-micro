/*
 * Root CA for TLS certificate verification in the WiFi example.
 *
 * This file is tracked (unlike secrets.h) because a root certificate is public, not a
 * credential. main.cpp includes it and hands it to WiFiTransport::set_ca_cert(), so the
 * example verifies TLS against the public Sentry cloud with no setup.
 *
 * Why this exists — read this before you change it:
 *
 *   WiFiTransport sends with TLS certificate verification OFF by default: the connection
 *   is encrypted but *unauthenticated*, so a device will talk to any server that answers
 *   its DSN host (a captive portal, a MITM, a stale DNS entry). You should call
 *   set_ca_cert() before trusting it. That call needs a PEM root certificate that chains
 *   to the leaf certificate your ingest host actually presents.
 *
 *   The block below is DigiCert Global Root G2. Every `*.ingest.sentry.io` host currently
 *   presents a leaf issued by "DigiCert Global G2 TLS RSA SHA256 2020 CA1", which chains to
 *   this root — checked with `openssl s_client` against ingest hosts in both the US and EU
 *   regions as of 2026-08-20. (An earlier version of this file pinned ISRG Root X1, the
 *   Let's Encrypt root; that chain does not verify any current Sentry ingest cert and made
 *   this example's TLS handshake fail outright, independent of anything else — this file's
 *   history is that first rotation, not evidence of a prior one.)
 *
 *   If you self-host Sentry, or route through your own proxy or tunnel, replace this with
 *   the root that issued *that* host's certificate — the handshake is rejected otherwise,
 *   which is what verification *is*. Prefer a root (long-lived) over your host's leaf cert
 *   (short-lived): a pinned certificate that expires bricks reporting on every deployed
 *   device at the same moment, and a maker with no CI has no way to push a new one. That
 *   renewal trade is exactly why the SDK leaves verification off by default. It is also why
 *   pinning any single commercial root is still a soft spot: Sentry can rotate which CA it
 *   buys certificates from — this file has already needed updating once for exactly that —
 *   and a device with only the old root goes back to failing every handshake until it is
 *   reflashed.
 *
 * It is `static const`, so it lives in flash, not RAM — 1,294 bytes (914 bytes DER once
 * base64-decoded) against your OTA slot, zero against the heap. Turning verification on at
 * all also pulls in a few KB of mbedTLS verification code the insecure path skips — the
 * handshake is the footprint to budget for, not the cert itself.
 */
#pragma once

/* DigiCert Global Root G2 — https://cacerts.digicert.com/DigiCertGlobalRootG2.crt */
static const char SENTRY_INGEST_CA_CERT[]
    = "-----BEGIN CERTIFICATE-----\n"
      "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
      "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
      "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
      "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
      "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
      "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
      "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
      "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
      "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
      "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
      "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
      "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
      "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
      "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
      "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
      "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
      "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
      "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
      "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
      "MrY=\n"
      "-----END CERTIFICATE-----\n";

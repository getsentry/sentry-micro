"""
PlatformIO pre-script: turn environment variables into build-time macros.

Lets a real WiFi network and a real DSN reach the firmware without ever being written to a
file that could be committed. `secrets.h` is fine on a personal bench, but it is one
`git add -A` away from a public write key, and it cannot be used by CI at all.

    export SENTRY_MICRO_WIFI_SSID='My Network'
    export SENTRY_MICRO_WIFI_PASSWORD='hunter2'
    export SENTRY_MICRO_DSN='https://key@o0.ingest.sentry.io/1'
    pio run -e esp32dev -t upload

Precedence, highest first:

  1. these environment variables  (-D on the command line, so they win)
  2. examples/wifi_basic/src/secrets.h  (gitignored; must use #ifndef guards)
  3. the placeholders in main.cpp

Only variables that are actually set are injected, so an unset variable falls through to
the next level instead of clobbering it with an empty string.
"""

import os

Import("env")  # noqa: F821 — injected by PlatformIO/SCons

# Environment variable -> preprocessor macro.
SECRETS = (
    ("SENTRY_MICRO_WIFI_SSID", "WIFI_SSID"),
    ("SENTRY_MICRO_WIFI_PASSWORD", "WIFI_PASSWORD"),
    ("SENTRY_MICRO_DSN", "SENTRY_DSN"),
    # Not a secret, but the same environment-to-macro mechanism. Set by scripts/release.sh
    # so the firmware reports the identical build-id that gets stamped into the ELF.
    ("SENTRY_MICRO_BUILD_ID", "SENTRY_BUILD_ID_HEX"),
)

# Values that must never be echoed: the WiFi password, and the DSN (which embeds a key that
# can write to the Sentry project). Build logs get pasted into issues and CI output.
SENSITIVE = {"SENTRY_MICRO_WIFI_PASSWORD", "SENTRY_MICRO_DSN"}

defines = []
for var, macro in SECRETS:
    value = os.environ.get(var)
    if not value:
        continue
    # StringifyMacro handles the quoting/escaping needed to survive the shell and the
    # compiler command line; hand-rolled \" escaping breaks on spaces in an SSID.
    defines.append((macro, env.StringifyMacro(value)))  # noqa: F821
    shown = "<set>" if var in SENSITIVE else value
    print("env_secrets: %s <- %s (%s)" % (macro, var, shown))

if defines:
    env.Append(CPPDEFINES=defines)  # noqa: F821
else:
    print("env_secrets: no SENTRY_MICRO_* variables set; using secrets.h or placeholders")

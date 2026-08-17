/**
 * The one part of the C++ layer that is not a pure rename.
 *
 * Everything else in `sentry_micro.hpp` is an inline forwarder; this file exists because
 * `sentry::init()` also wires up a default debug logger, and doing that requires touching
 * Arduino's `Serial` — which is C++ and cannot live in `sentry_micro.c`.
 *
 * Named `_cxx` rather than sharing the `sentry_micro` basename with the C file: two
 * translation units with the same stem are a needless object-name collision risk across
 * build systems.
 */
#include "sentry_micro.h"

#if defined(ARDUINO)
#    include <Arduino.h>
#endif

namespace sentry {
namespace {

#if defined(ARDUINO)
/**
 * Default sink for SDK debug output on Arduino.
 *
 * Uses `Serial` rather than `printf` deliberately: on the native-USB parts (S2, S3, C3,
 * C6) `printf` goes to UART0 while `Serial` is the USB CDC endpoint the developer
 * actually has open, so a `printf`-based default would log to a pin nobody is watching.
 */
void serial_logger(const char *message)
{
    Serial.print("[sentry] ");
    Serial.println(message);
}
#endif

} // namespace

bool init(const Options &options)
{
#if defined(ARDUINO)
    /* Only when asked for, and never over a logger the user chose themselves. */
    if (options.debug && sentry_get_logger() == nullptr) {
        sentry_set_logger(&serial_logger);
    }
#endif
    return sentry_init(&options);
}

} // namespace sentry

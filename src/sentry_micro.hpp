/**
 * C++ ergonomics over the C API in `sentry_micro.h`.
 *
 * Included automatically by `sentry_micro.h` when compiling as C++ — you do not include
 * this directly. Everything here is an inline wrapper: there is no second copy of SDK
 * state, and no C++ object needs to exist for the SDK to work. Deleting this file would
 * cost spelling, not capability.
 */
#ifndef SENTRY_MICRO_HPP_INCLUDED
#define SENTRY_MICRO_HPP_INCLUDED

#ifndef SENTRY_MICRO_H_INCLUDED
#    error "include <sentry_micro.h>, which pulls in this header for C++ builds"
#endif

#include "transport/sentry_transport.hpp"

namespace sentry {

/**
 * `sentry_options_t` with the defaults already applied.
 *
 * Derives from the C struct rather than duplicating it, so the two cannot drift and no
 * conversion happens at the boundary:
 *
 *     sentry::Options options;
 *     options.dsn = "...";
 *     sentry::init(options);
 */
struct Options : sentry_options_t {
    Options() { sentry_options_defaults(this); }
};

/**
 * Initialise the SDK.
 *
 * Also installs the Arduino `Serial` logger when `options.debug` is set and none has been
 * installed yet — the reason a C++ user gets debug output without wiring one up, and the
 * one behaviour in this header that is not a pure rename. Call `sentry_set_logger()`
 * yourself beforehand to override it.
 */
bool init(const Options &options);

inline void shutdown() { sentry_close(); }
inline bool is_enabled() { return sentry_is_enabled(); }
inline const char *sdk_version() { return sentry_sdk_version(); }
inline const sentry_dsn_t &dsn() { return *sentry_get_dsn(); }
inline const char *envelope_url() { return sentry_get_envelope_url(); }
inline const char *auth_header() { return sentry_get_auth_header(); }
inline const char *org_id() { return sentry_get_org_id(); }
inline const sentry_device_info_t &device_info() { return *sentry_get_device_info(); }
inline const sentry_options_t &options() { return *sentry_get_options(); }

/** Register a transport. Must outlive the SDK — a file-scope object, not a stack local. */
inline void set_transport(const Transport &transport)
{
    sentry_set_transport(transport.c_transport());
}

/** Detach the current transport; delivery then reports `SEND_UNAVAILABLE`. */
inline void clear_transport() { sentry_set_transport(nullptr); }

} // namespace sentry

#endif /* SENTRY_MICRO_HPP_INCLUDED */

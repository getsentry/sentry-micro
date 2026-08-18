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

/**
 * Adopt the trace context arriving with a request.
 *
 *     sentry::trace_adopt(sentry_trace_header, baggage_header);
 *     handle();
 *     sentry::trace_release();
 *
 * Release it when the operation ends — see `sentry_trace_adopt()` for why holding it is
 * actively harmful rather than merely untidy.
 */
inline bool trace_adopt(const char *sentry_trace, const char *baggage = nullptr)
{
    return sentry_trace_adopt(sentry_trace, baggage);
}

/** Begin a trace this device originates — a boot, an OTA. */
inline bool trace_start() { return sentry_trace_start(); }

/** End the active trace. Safe when none is active. */
inline void trace_release() { sentry_trace_release(); }

/** The active trace; its `active` field is false when nothing is in flight. */
inline const sentry_trace_context_t &trace() { return *sentry_trace_current(); }

/** Write a `sentry-trace` value for an outbound call. 0 means "send no header". */
inline size_t trace_header(char *buf, size_t cap) { return sentry_trace_header(buf, cap); }

/**
 * Report a message.
 *
 *     sentry::capture_message(SENTRY_LEVEL_WARNING, "OTA aborted: bad signature");
 *
 * Argument order mirrors the C call exactly rather than putting the text first with a
 * defaulted level, because two spellings of the same call in one codebase is worse than
 * one extra word. Subject to the throttle; see `sentry_capture_message()`.
 */
inline sentry_response_t capture_message(sentry_level_t level, const char *message)
{
    return sentry_capture_message(level, message);
}

/** Captured messages dropped by the local throttle since init. */
inline uint32_t suppressed_count() { return sentry_suppressed_count(); }

/** Register a transport. Must outlive the SDK — a file-scope object, not a stack local. */
inline void set_transport(const Transport &transport)
{
    sentry_set_transport(transport.c_transport());
}

/** Detach the current transport; delivery then reports `SEND_UNAVAILABLE`. */
inline void clear_transport() { sentry_set_transport(nullptr); }

} // namespace sentry

#endif /* SENTRY_MICRO_HPP_INCLUDED */

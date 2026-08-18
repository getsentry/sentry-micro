/**
 * AutoTransport — pick a route per delivery attempt.
 *
 * Holds an ordered list of other transports and, on every call, delegates to the first one
 * whose `is_available()` is true:
 *
 *     static sentry::WiFiTransport wifi;
 *     static sentry::SerialTransport serial;
 *     static sentry::AutoTransport transport({&wifi, &serial});
 *     sentry::set_transport(transport);
 *
 * The selection is re-made on every `send()`/`is_available()` call, not cached at
 * construction — WiFi comes and goes, and a BLE central can connect long after boot. That
 * is the entire reason this class exists rather than the one-time `if (connected) ... else
 * ...` it replaces: a device that failed to associate at boot and picked the serial relay
 * would be stuck on it for the rest of that boot, even once WiFi came up ten seconds later.
 *
 * **Ordering matters, in a way that is easy to get backwards.** `SerialTransport` has no
 * way to detect a listener on a bare UART, so its `is_available()` always returns true —
 * it only discovers the truth via its own timeout inside `send()`. Anything placed *after*
 * `SerialTransport` in this list is therefore unreachable: this always picks the first
 * available one, and Serial always claims to be. Put transports that can tell the truth
 * about availability before ones that cannot: `{&wifi, &relay, &serial}`, never
 * `{&wifi, &serial, &relay}`.
 *
 * Has no Arduino or ESP-IDF dependency of its own — it only calls the abstract `Transport`
 * interface — so it is usable from a pure ESP-IDF C++ build, and is host-testable against
 * fake transports (see test/test_transport_auto).
 */
#ifndef SENTRY_MICRO_TRANSPORT_AUTO_HPP_INCLUDED
#define SENTRY_MICRO_TRANSPORT_AUTO_HPP_INCLUDED

#include <initializer_list>

#include "sentry_transport.hpp"

namespace sentry {

class AutoTransport : public Transport {
public:
    /**
     * How many transports one instance can hold. A fixed bound rather than a heap
     * container, matching the no-allocation rule everywhere else in this SDK. There are
     * three concrete transports today (WiFi, serial, relay); four is headroom, not a real
     * constraint.
     */
    static constexpr size_t MAX_TRANSPORTS = 4;

    /**
     * @param transports Ordered, most-preferred first. Each must outlive this object —
     *                    the same rule as every other pointer this SDK stores. Pointer
     *                    *values* are copied out of the initializer list here; the list's
     *                    own backing storage does not live past this constructor call, so
     *                    nothing keeps a reference into it.
     */
    AutoTransport(std::initializer_list<Transport *> transports)
    {
        for (Transport *t : transports) {
            if (count_ < MAX_TRANSPORTS) {
                transports_[count_++] = t;
            }
        }
    }

    Response send(const char *url, const Headers &headers, const uint8_t *body, size_t len) override
    {
        Transport *chosen = select();
        return chosen ? chosen->send(url, headers, body, len) : Response(SEND_UNAVAILABLE);
    }

    bool is_available() override { return select() != nullptr; }

    /**
     * The name of whichever transport was actually last selected, not a static "auto" —
     * so the core's own debug log ("sent %u bytes via %s: result %d") says `wifi` or
     * `serial` per attempt, for free, with no changes needed anywhere else. Reads back as
     * "auto" before the first selection and after any attempt that found nothing available,
     * so the log never names a transport that was not the one tried.
     */
    const char *name() const override { return last_selected_ ? last_selected_->name() : "auto"; }

private:
    Transport *select()
    {
        /* Cleared up front, not only assigned on a hit: a scan that finds nothing must not
         * leave name() reporting the route from a previous attempt. The core logs the name
         * next to the result ("sent %u bytes via %s: result %d"), so a stale one would
         * pin a SEND_UNAVAILABLE on a transport that was never tried — precisely the
         * everything-is-down case this class exists to handle. */
        last_selected_ = nullptr;
        for (size_t i = 0; i < count_; i++) {
            if (transports_[i]->is_available()) {
                last_selected_ = transports_[i];
                return transports_[i];
            }
        }
        return nullptr;
    }

    Transport *transports_[MAX_TRANSPORTS] = { };
    size_t count_ = 0;
    Transport *last_selected_ = nullptr;
};

} // namespace sentry

#endif /* SENTRY_MICRO_TRANSPORT_AUTO_HPP_INCLUDED */

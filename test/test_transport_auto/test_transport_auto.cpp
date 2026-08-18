/**
 * Host tests for AutoTransport's selection logic.
 *
 * AutoTransport has no Arduino or ESP-IDF dependency of its own — it only calls the
 * abstract Transport interface — so it is testable here against fake transports, the same
 * way the buffer's ring logic is tested against a fake storage vtable. What matters is the
 * *policy*: pick the first available one, skip unavailable ones in order, report
 * SEND_UNAVAILABLE when none are, and re-evaluate fresh on every call.
 */

#include <cstring>
#include <unity.h>

#include "sentry_transport_auto.hpp"

using namespace sentry;

namespace {

/** A transport whose availability and outcome are set directly by the test. */
class FakeTransport : public Transport {
public:
    FakeTransport(const char *name, bool available, SendResult result)
        : name_(name)
        , available_(available)
        , result_(result)
    {
    }

    Response send(const char *, const Headers &, const uint8_t *, size_t) override
    {
        send_count_++;
        return Response(result_);
    }

    bool is_available() override { return available_; }
    const char *name() const override { return name_; }

    void set_available(bool available) { available_ = available; }
    int send_count() const { return send_count_; }

private:
    const char *name_;
    bool available_;
    SendResult result_;
    int send_count_ = 0;
};

const Headers EMPTY_HEADERS = { "", "" };

} // namespace

void setUp(void) { }
void tearDown(void) { }

static void test_picks_the_first_available_transport(void)
{
    FakeTransport wifi("wifi", true, SEND_OK);
    FakeTransport serial("serial", true, SEND_OK);
    AutoTransport transport({ &wifi, &serial });

    Response r = transport.send("url", EMPTY_HEADERS, nullptr, 0);
    TEST_ASSERT_EQUAL(SEND_OK, r.result);
    TEST_ASSERT_EQUAL_STRING("wifi", transport.name());
    TEST_ASSERT_EQUAL(1, wifi.send_count());
    TEST_ASSERT_EQUAL(0, serial.send_count());
}

static void test_skips_unavailable_transports_in_order(void)
{
    FakeTransport wifi("wifi", false, SEND_OK);
    FakeTransport serial("serial", true, SEND_OK);
    AutoTransport transport({ &wifi, &serial });

    Response r = transport.send("url", EMPTY_HEADERS, nullptr, 0);
    TEST_ASSERT_EQUAL(SEND_OK, r.result);
    TEST_ASSERT_EQUAL_STRING("serial", transport.name());
    TEST_ASSERT_EQUAL(0, wifi.send_count());
    TEST_ASSERT_EQUAL(1, serial.send_count());
}

static void test_reports_unavailable_when_nothing_is(void)
{
    FakeTransport wifi("wifi", false, SEND_OK);
    FakeTransport serial("serial", false, SEND_OK);
    AutoTransport transport({ &wifi, &serial });

    TEST_ASSERT_FALSE(transport.is_available());
    Response r = transport.send("url", EMPTY_HEADERS, nullptr, 0);
    TEST_ASSERT_EQUAL(SEND_UNAVAILABLE, r.result);
    TEST_ASSERT_EQUAL(0, wifi.send_count());
    TEST_ASSERT_EQUAL(0, serial.send_count());
}

static void test_name_reports_auto_before_anything_is_ever_selected(void)
{
    FakeTransport wifi("wifi", false, SEND_OK);
    AutoTransport transport({ &wifi });

    TEST_ASSERT_EQUAL_STRING("auto", transport.name());
}

static void test_reevaluates_on_every_call_rather_than_caching(void)
{
    FakeTransport wifi("wifi", false, SEND_OK);
    FakeTransport serial("serial", true, SEND_OK);
    AutoTransport transport({ &wifi, &serial });

    transport.send("url", EMPTY_HEADERS, nullptr, 0);
    TEST_ASSERT_EQUAL_STRING("serial", transport.name());

    /* WiFi associates mid-session — no reboot, no re-construction, just a later call. */
    wifi.set_available(true);
    transport.send("url", EMPTY_HEADERS, nullptr, 0);
    TEST_ASSERT_EQUAL_STRING("wifi", transport.name());
    TEST_ASSERT_EQUAL(1, wifi.send_count());
    TEST_ASSERT_EQUAL(1, serial.send_count());
}

static void test_a_failed_send_from_the_chosen_transport_is_returned_as_is(void)
{
    /* AutoTransport does not fall through to the next transport within one attempt if the
     * chosen one's send() itself fails — that failure is buffered and retried by the core,
     * which will call send() again later and re-select fresh. */
    FakeTransport wifi("wifi", true, SEND_ERROR);
    FakeTransport serial("serial", true, SEND_OK);
    AutoTransport transport({ &wifi, &serial });

    Response r = transport.send("url", EMPTY_HEADERS, nullptr, 0);
    TEST_ASSERT_EQUAL(SEND_ERROR, r.result);
    TEST_ASSERT_EQUAL(1, wifi.send_count());
    TEST_ASSERT_EQUAL(0, serial.send_count());
}

static void test_single_transport_list(void)
{
    FakeTransport wifi("wifi", true, SEND_OK);
    AutoTransport transport({ &wifi });

    Response r = transport.send("url", EMPTY_HEADERS, nullptr, 0);
    TEST_ASSERT_EQUAL(SEND_OK, r.result);
    TEST_ASSERT_EQUAL_STRING("wifi", transport.name());
}

static void test_empty_list_is_always_unavailable(void)
{
    AutoTransport transport({ });

    TEST_ASSERT_FALSE(transport.is_available());
    Response r = transport.send("url", EMPTY_HEADERS, nullptr, 0);
    TEST_ASSERT_EQUAL(SEND_UNAVAILABLE, r.result);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_picks_the_first_available_transport);
    RUN_TEST(test_skips_unavailable_transports_in_order);
    RUN_TEST(test_reports_unavailable_when_nothing_is);
    RUN_TEST(test_name_reports_auto_before_anything_is_ever_selected);
    RUN_TEST(test_reevaluates_on_every_call_rather_than_caching);
    RUN_TEST(test_a_failed_send_from_the_chosen_transport_is_returned_as_is);
    RUN_TEST(test_single_transport_list);
    RUN_TEST(test_empty_list_is_always_unavailable);
    return UNITY_END();
}

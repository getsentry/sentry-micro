/**
 * Host tests for the DSN parser.
 *
 * The parser is the one piece of the SDK where a subtle bug is silent and total: a
 * mis-parsed host means every event a fleet ever produces goes to the wrong place (or
 * nowhere), and there is no console on the device to notice it. So it gets tested against
 * the real shapes — sentry.io, self-hosted behind a path prefix, explicit ports, legacy
 * secret keys — plus the malformed inputs that must leave the SDK disabled rather than
 * half-configured.
 */

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "sentry_dsn.h"

#define SENTRY_IO_DSN "https://abc123def456@o1234.ingest.us.sentry.io/4507"

void setUp(void) { }
void tearDown(void) { }

static void test_parses_a_sentry_io_dsn(void)
{
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, SENTRY_IO_DSN));

    TEST_ASSERT_TRUE(dsn.valid);
    TEST_ASSERT_TRUE(dsn.is_secure);
    TEST_ASSERT_EQUAL_STRING("abc123def456", dsn.public_key);
    TEST_ASSERT_EQUAL_STRING("o1234.ingest.us.sentry.io", dsn.host);
    TEST_ASSERT_EQUAL_STRING("4507", dsn.project_id);
    TEST_ASSERT_EQUAL_STRING("", dsn.path);
    TEST_ASSERT_EQUAL_UINT16(0, dsn.port);
}

static void test_builds_the_envelope_url(void)
{
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, SENTRY_IO_DSN));

    char url[SENTRY_MICRO_MAX_URL_LEN];
    TEST_ASSERT_TRUE(sentry_dsn_envelope_url(&dsn, url, sizeof(url)) > 0);
    /* No `:443` — the port is emitted only when the DSN spelled one out. */
    TEST_ASSERT_EQUAL_STRING("https://o1234.ingest.us.sentry.io/api/4507/envelope/", url);
}

static void test_builds_the_auth_header(void)
{
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, SENTRY_IO_DSN));

    char auth[SENTRY_MICRO_MAX_AUTH_LEN];
    TEST_ASSERT_TRUE(sentry_dsn_auth_header(&dsn, auth, sizeof(auth)) > 0);
    TEST_ASSERT_EQUAL_STRING("Sentry sentry_version=7, sentry_client=" SENTRY_MICRO_SDK_USER_AGENT
                             ", sentry_key=abc123def456",
        auth);
    /* The key belongs in the header, never in the URL. */
    TEST_ASSERT_NULL(strstr(auth, "sentry_timestamp"));
}

static void test_parses_explicit_port_and_plain_http(void)
{
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, "http://key@192.168.1.50:9000/2"));

    TEST_ASSERT_FALSE(dsn.is_secure);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", dsn.host);
    TEST_ASSERT_EQUAL_UINT16(9000, dsn.port);

    char url[SENTRY_MICRO_MAX_URL_LEN];
    sentry_dsn_envelope_url(&dsn, url, sizeof(url));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.50:9000/api/2/envelope/", url);
}

static void test_parses_self_hosted_path_prefix(void)
{
    /* Self-hosted Sentry behind a reverse proxy: the prefix must survive into the URL. */
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, "https://key@example.com/sentry/42"));

    TEST_ASSERT_EQUAL_STRING("example.com", dsn.host);
    TEST_ASSERT_EQUAL_STRING("/sentry", dsn.path);
    TEST_ASSERT_EQUAL_STRING("42", dsn.project_id);

    char url[SENTRY_MICRO_MAX_URL_LEN];
    sentry_dsn_envelope_url(&dsn, url, sizeof(url));
    TEST_ASSERT_EQUAL_STRING("https://example.com/sentry/api/42/envelope/", url);
}

static void test_drops_the_legacy_secret_key(void)
{
    /* Old DSNs carried `public:secret@`. Ingest ignores the secret; so do we. */
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, "https://pub:sec@sentry.io/7"));
    TEST_ASSERT_EQUAL_STRING("pub", dsn.public_key);
}

static void test_tolerates_a_trailing_slash(void)
{
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, "https://key@sentry.io/7/"));
    TEST_ASSERT_EQUAL_STRING("7", dsn.project_id);
}

static void test_rejects_malformed_dsns(void)
{
    static const char *bad[] = {
        NULL,
        "",
        "not-a-url",
        "ftp://key@sentry.io/1", /* unsupported scheme */
        "https://sentry.io/1", /* no public key */
        "https://@sentry.io/1", /* empty public key */
        "https://key@sentry.io", /* no project id */
        "https://key@sentry.io/", /* empty project id */
        "https://key@/1", /* no host */
        "https://key@sentry.io/abc", /* non-numeric project id */
        "https://key@sentry.io:0/1", /* invalid port */
        "https://key@sentry.io:99999/1", /* port out of range */
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        sentry_dsn_t dsn;
        TEST_ASSERT_FALSE_MESSAGE(sentry_dsn_parse(&dsn, bad[i]), bad[i] ? bad[i] : "(null)");
        /* A rejected DSN must leave the struct unusable, not partially filled. */
        TEST_ASSERT_FALSE(dsn.valid);
    }
}

static void test_rejects_oversized_fields_rather_than_truncating(void)
{
    /* Silently truncating a hostname would point a whole fleet at the wrong server. */
    char long_host[SENTRY_MICRO_MAX_DSN_LEN];
    int n = snprintf(long_host, sizeof(long_host), "https://key@");
    memset(long_host + n, 'a', SENTRY_MICRO_MAX_HOST_LEN + 4);
    snprintf(long_host + n + SENTRY_MICRO_MAX_HOST_LEN + 4,
        sizeof(long_host) - n - SENTRY_MICRO_MAX_HOST_LEN - 4, "/1");

    sentry_dsn_t dsn;
    TEST_ASSERT_FALSE(sentry_dsn_parse(&dsn, long_host));
}

static void test_output_helpers_refuse_an_invalid_dsn(void)
{
    sentry_dsn_t dsn;
    sentry_dsn_parse(&dsn, "garbage");

    char buf[SENTRY_MICRO_MAX_URL_LEN];
    TEST_ASSERT_EQUAL_size_t(0, sentry_dsn_envelope_url(&dsn, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, sentry_dsn_auth_header(&dsn, buf, sizeof(buf)));

    /* And refuse a buffer they cannot fill completely, rather than emitting a half URL. */
    sentry_dsn_parse(&dsn, SENTRY_IO_DSN);
    char tiny[8];
    TEST_ASSERT_EQUAL_size_t(0, sentry_dsn_envelope_url(&dsn, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", tiny);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parses_a_sentry_io_dsn);
    RUN_TEST(test_builds_the_envelope_url);
    RUN_TEST(test_builds_the_auth_header);
    RUN_TEST(test_parses_explicit_port_and_plain_http);
    RUN_TEST(test_parses_self_hosted_path_prefix);
    RUN_TEST(test_drops_the_legacy_secret_key);
    RUN_TEST(test_tolerates_a_trailing_slash);
    RUN_TEST(test_rejects_malformed_dsns);
    RUN_TEST(test_rejects_oversized_fields_rather_than_truncating);
    RUN_TEST(test_output_helpers_refuse_an_invalid_dsn);
    return UNITY_END();
}

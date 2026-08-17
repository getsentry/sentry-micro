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

static void test_recovers_org_id_from_the_host(void)
{
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, SENTRY_IO_DSN));
    TEST_ASSERT_EQUAL_STRING("1234", dsn.org_id);

    /* It is metadata for trace propagation, not routing — the URL must not change. */
    char url[SENTRY_MICRO_MAX_URL_LEN];
    sentry_dsn_envelope_url(&dsn, url, sizeof(url));
    TEST_ASSERT_EQUAL_STRING("https://o1234.ingest.us.sentry.io/api/4507/envelope/", url);
}

static void test_leaves_org_id_empty_when_the_host_has_none(void)
{
    /* Absence is normal — self-hosted and custom ingest domains carry no `o<digits>.`. */
    static const char *no_org[] = {
        "https://key@sentry.io/1", /* no leading 'o' */
        "https://key@example.com/1",
        "https://key@o.ingest.sentry.io/1", /* 'o' but no digits */
        "https://key@oabc.ingest.sentry.io/1", /* 'o' but not numeric */
        "https://key@o12ab.ingest.sentry.io/1", /* partially numeric — must not pass */
        "https://key@o1234/1", /* no dot at all */
        "https://key@o123456789012345678901.ingest.sentry.io/1", /* 21 digits, > u64 */
    };

    for (size_t i = 0; i < sizeof(no_org) / sizeof(no_org[0]); i++) {
        sentry_dsn_t dsn;
        TEST_ASSERT_TRUE_MESSAGE(sentry_dsn_parse(&dsn, no_org[i]), no_org[i]);
        /* An absent org id must not invalidate the DSN — events still send fine. */
        TEST_ASSERT_TRUE(dsn.valid);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("", dsn.org_id, no_org[i]);
    }
}

static void test_resolves_org_id_with_an_override(void)
{
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, SENTRY_IO_DSN));

    /* Explicit configuration beats what the host implies. */
    TEST_ASSERT_EQUAL_STRING("999", sentry_dsn_resolve_org_id(&dsn, "999"));
    /* An unset or blank override falls through to the DSN. */
    TEST_ASSERT_EQUAL_STRING("1234", sentry_dsn_resolve_org_id(&dsn, NULL));
    TEST_ASSERT_EQUAL_STRING("1234", sentry_dsn_resolve_org_id(&dsn, ""));

    /* Self-hosted: nothing in the host, so the override is the only source. */
    sentry_dsn_t self_hosted;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&self_hosted, "https://key@sentry.example.com/1"));
    TEST_ASSERT_EQUAL_STRING("42", sentry_dsn_resolve_org_id(&self_hosted, "42"));
    TEST_ASSERT_EQUAL_STRING("", sentry_dsn_resolve_org_id(&self_hosted, NULL));

    /* Never NULL, even with nothing to go on. */
    sentry_dsn_t bad;
    sentry_dsn_parse(&bad, "garbage");
    TEST_ASSERT_EQUAL_STRING("", sentry_dsn_resolve_org_id(&bad, NULL));
    TEST_ASSERT_EQUAL_STRING("", sentry_dsn_resolve_org_id(NULL, NULL));
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

static void test_matches_a_url_host_exactly(void)
{
    TEST_ASSERT_TRUE(sentry_url_host_matches("https://sentry.io/api/1/envelope/", "sentry.io"));
    TEST_ASSERT_TRUE(sentry_url_host_matches("http://sentry.io:9000/api/1/envelope/", "sentry.io"));
    /* Hostnames are case-insensitive. */
    TEST_ASSERT_TRUE(sentry_url_host_matches("https://SenTry.IO/api/1/", "sentry.io"));
    /* No path at all is still a valid host to compare. */
    TEST_ASSERT_TRUE(sentry_url_host_matches("https://sentry.io", "sentry.io"));
}

static void test_rejects_hosts_that_merely_resemble_the_target(void)
{
    /* Each of these would pass a sloppy prefix/suffix/substring comparison. A transport
     * that accepted any of them would POST a device's data to someone else's server — and
     * for the relay transport, would make a user's phone do it. */
    static const char *hostile[] = {
        "https://evil-sentry.io/api/1/", /* prefix trick */
        "https://sentry.io.evil.com/api/1/", /* suffix trick */
        "https://notsentry.io/api/1/",
        "https://sentry.i/api/1/", /* shorter */
        "https://sentry.ioo/api/1/", /* longer */
        "https://sentry.io@evil.com/api/1/", /* userinfo: real host is evil.com */
        "https://user:pass@evil.com/sentry.io", /* userinfo again */
        "sentry.io/api/1/", /* no scheme */
        "", /* empty */
    };
    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(sentry_url_host_matches(hostile[i], "sentry.io"), hostile[i]);
    }

    TEST_ASSERT_FALSE(sentry_url_host_matches(NULL, "sentry.io"));
    TEST_ASSERT_FALSE(sentry_url_host_matches("https://sentry.io/", NULL));
    TEST_ASSERT_FALSE(sentry_url_host_matches("https://sentry.io/", ""));
}

static void test_matches_the_host_from_a_parsed_dsn(void)
{
    /* The whole point: what the DSN says and what a transport is allowed to reach agree. */
    sentry_dsn_t dsn;
    TEST_ASSERT_TRUE(sentry_dsn_parse(&dsn, SENTRY_IO_DSN));

    char url[SENTRY_MICRO_MAX_URL_LEN];
    sentry_dsn_envelope_url(&dsn, url, sizeof(url));
    TEST_ASSERT_TRUE(sentry_url_host_matches(url, dsn.host));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_matches_a_url_host_exactly);
    RUN_TEST(test_rejects_hosts_that_merely_resemble_the_target);
    RUN_TEST(test_matches_the_host_from_a_parsed_dsn);
    RUN_TEST(test_parses_a_sentry_io_dsn);
    RUN_TEST(test_builds_the_envelope_url);
    RUN_TEST(test_builds_the_auth_header);
    RUN_TEST(test_recovers_org_id_from_the_host);
    RUN_TEST(test_leaves_org_id_empty_when_the_host_has_none);
    RUN_TEST(test_resolves_org_id_with_an_override);
    RUN_TEST(test_parses_explicit_port_and_plain_http);
    RUN_TEST(test_parses_self_hosted_path_prefix);
    RUN_TEST(test_drops_the_legacy_secret_key);
    RUN_TEST(test_tolerates_a_trailing_slash);
    RUN_TEST(test_rejects_malformed_dsns);
    RUN_TEST(test_rejects_oversized_fields_rather_than_truncating);
    RUN_TEST(test_output_helpers_refuse_an_invalid_dsn);
    return UNITY_END();
}

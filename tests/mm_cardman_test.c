/*
 * Offline unit tests for the CARDMAN glue (src/mm_cardman.c). No bridge
 * needed: the bridge-facing paths are covered by tests/mm_cardman_drive.c
 * and tools/test_cardman_bridge.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/mm_cardman.h"
#include "src/mm_cardman_bridge.h"

static void set_tz(const char *tz) {
#ifdef _WIN32
    char env[64];

    snprintf(env, sizeof(env), "TZ=%s", tz);
    _putenv(env);
    _tzset();
#else
    setenv("TZ", tz, 1);
    tzset();
#endif
}

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void test_auth_code_codec(void) {
    const char *good[] = { "A1B2C3D4", "00000001", "ZZZZZZZZ", "0Q7KX2LM" };
    const char *bad[]  = { "bogus", "", "123", "A1B2C3D4X", "a1b2c3d4", "A1B2C3D-", "A1B2 3D4" };
    uint64_t v;
    char out[16];

    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        CHECK(cardman_auth_code_decode(good[i], &v) == 0);
        CHECK(v < CARDMAN_AUTH_CODE_LIMIT);
        CHECK(cardman_auth_code_encode(v, out, sizeof(out)) == 0);
        CHECK(strcmp(out, good[i]) == 0);
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(cardman_auth_code_decode(bad[i], &v) == -1);
    }
    CHECK(cardman_auth_code_decode(NULL, &v) == -1);

    /* Codes past 32 bits must survive the round trip (the old hex path
     * kept only the low 32 bits).
     */
    CHECK(cardman_auth_code_decode("ZZZZZZZZ", &v) == 0);
    CHECK(v == CARDMAN_AUTH_CODE_LIMIT - 1);
    CHECK(v > 0xFFFFFFFFULL);

    CHECK(cardman_auth_code_encode(0, out, sizeof(out)) == -1);
    CHECK(cardman_auth_code_encode(CARDMAN_AUTH_CODE_LIMIT, out, sizeof(out)) == -1);
    CHECK(cardman_auth_code_encode(1, out, 8) == -1);
}

static void test_requested_at(void) {
    char a[24], b[24], c[24], d[24];
    time_t t0 = 1791916975;     /* 2026-10-13T18:42:55Z */

    CHECK(cardman_requested_at("2125551000", 7, t0, a, sizeof(a)) == 0);
    CHECK(strcmp(a, "2026-10-13T18:42:55Z") == 0);

    /* A retry seconds later reuses it byte for byte. */
    CHECK(cardman_requested_at("2125551000", 7, t0 + 30, b, sizeof(b)) == 0);
    CHECK(strcmp(a, b) == 0);

    /* Another seq or terminal gets its own. */
    CHECK(cardman_requested_at("2125551000", 8, t0 + 31, c, sizeof(c)) == 0);
    CHECK(strcmp(c, "2026-10-13T18:43:26Z") == 0);
    CHECK(cardman_requested_at("2125552000", 7, t0 + 32, c, sizeof(c)) == 0);
    CHECK(strcmp(c, "2026-10-13T18:43:27Z") == 0);

    /* Past the TTL a reused seq (a wrap, or a terminal reset) is a new auth. */
    CHECK(cardman_requested_at("2125551000", 7, t0 + CARDMAN_REQUESTED_AT_TTL_SEC, d, sizeof(d)) == 0);
    CHECK(strcmp(a, d) != 0);
}

static void test_terminal_ts(void) {
    /* 2026-10-13 18:42:55 local; in UTC (TZ set by main) it is unchanged. */
    uint8_t ts[6]  = { 126, 10, 13, 18, 42, 55 };
    uint8_t bad[6] = { 126, 13, 1, 0, 0, 0 };
    char out[24];

    CHECK(cardman_terminal_ts_to_utc(ts, out, sizeof(out)) == 0);
    CHECK(strcmp(out, "2026-10-13T18:42:55Z") == 0);
    CHECK(cardman_terminal_ts_to_utc(bad, out, sizeof(out)) == -1);

    /* Dates mktime() would silently normalize are refused. */
    {
        uint8_t feb31[6]  = { 126, 2, 31, 12, 0, 0 };
        uint8_t feb29[6]  = { 126, 2, 29, 12, 0, 0 };    /* 2026 is not a leap year */
        uint8_t apr31[6]  = { 126, 4, 31, 12, 0, 0 };
        uint8_t leap29[6] = { 128, 2, 29, 12, 0, 0 };    /* 2028 is */
        uint8_t leapsec[6] = { 126, 12, 31, 23, 59, 60 };

        CHECK(cardman_terminal_ts_to_utc(feb31, out, sizeof(out)) == -1);
        CHECK(cardman_terminal_ts_to_utc(feb29, out, sizeof(out)) == -1);
        CHECK(cardman_terminal_ts_to_utc(apr31, out, sizeof(out)) == -1);
        CHECK(cardman_terminal_ts_to_utc(leap29, out, sizeof(out)) == 0);
        CHECK(strcmp(out, "2028-02-29T12:00:00Z") == 0);
        /* A leap second is the next second, not a refused CDR. */
        CHECK(cardman_terminal_ts_to_utc(leapsec, out, sizeof(out)) == 0);
        CHECK(strcmp(out, "2027-01-01T00:00:00Z") == 0);
    }

    /* Off UTC the conversion must actually shift: 18:42:55 EDT is 22:42:55Z.
     * POSIX TZ strings need no tzdata; MSVC takes the short form.
     */
#ifdef _WIN32
    set_tz("EST5EDT");
#else
    set_tz("EST5EDT,M3.2.0,M11.1.0");
#endif
    CHECK(cardman_terminal_ts_to_utc(ts, out, sizeof(out)) == 0);
    CHECK(strcmp(out, "2026-10-13T22:42:55Z") == 0);
    set_tz("UTC");
}

static int body_of(const char *response, char *body, size_t body_len) {
    return cardman_bridge_http_body(response, strlen(response), body, body_len);
}

static void test_bridge_response(void) {
    char body[256];
    const char *ok = "{\"approved\": true, \"disposition\": \"approved\", \"auth_code\": \"0Q7KX2LM\"}";
    char resp[512];

    /* Strict JSON: one flat object, unique keys, nothing trailing. */
    CHECK(cardman_bridge_json_flat_ok(ok) == 1);
    CHECK(cardman_bridge_json_flat_ok("{\"delivered\": false, \"retryable\": true, \"state\": null}") == 1);
    CHECK(cardman_bridge_json_flat_ok("{\"delivered\":true,\"charged_cents\":-0}") == 1);
    CHECK(cardman_bridge_json_flat_ok("{}") == 1);
    CHECK(cardman_bridge_json_flat_ok("{\"approved\": true, \"auth_code\": \"0Q7K") == 0);       /* truncated */
    CHECK(cardman_bridge_json_flat_ok("{\"approved\": true}garbage") == 0);
    CHECK(cardman_bridge_json_flat_ok("{\"approved\": true,}") == 0);
    CHECK(cardman_bridge_json_flat_ok("{\"approved\": false, \"approved\": true}") == 0);       /* duplicate */
    CHECK(cardman_bridge_json_flat_ok("{\"x\": {\"approved\": true}}") == 0);                   /* nested */
    CHECK(cardman_bridge_json_flat_ok("{\"x\": \"a\\\"approved\\\":true\"}") == 0);              /* escapes */
    CHECK(cardman_bridge_json_flat_ok("{\"approved\": tru}") == 0);
    CHECK(cardman_bridge_json_flat_ok("[\"approved\"]") == 0);
    CHECK(cardman_bridge_json_flat_ok("") == 0);

    /* HTTP framing: 200, Content-Length exactly the body. */
    snprintf(resp, sizeof(resp), "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(ok), ok);
    CHECK(body_of(resp, body, sizeof(body)) == 0);
    CHECK(strcmp(body, ok) == 0);
    snprintf(resp, sizeof(resp), "HTTP/1.1 200\r\nContent-Length: %zu\r\n\r\n%s", strlen(ok), ok);
    CHECK(body_of(resp, body, sizeof(body)) == 0);

    /* A truncated body (fewer bytes than announced) is refused, even
     * though the part received says approved.
     */
    snprintf(resp, sizeof(resp), "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n{\"approved\": true, \"auth_code\": \"0Q7KX2LM\"",
             strlen(ok) + 40);
    CHECK(body_of(resp, body, sizeof(body)) == -1);
    snprintf(resp, sizeof(resp), "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n%s", ok);   /* extra bytes */
    CHECK(body_of(resp, body, sizeof(body)) == -1);
    snprintf(resp, sizeof(resp), "HTTP/1.0 200 OK\r\n\r\n%s", ok);                      /* no length */
    CHECK(body_of(resp, body, sizeof(body)) == -1);
    snprintf(resp, sizeof(resp), "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(ok), strlen(ok), ok);
    CHECK(body_of(resp, body, sizeof(body)) == -1);
    snprintf(resp, sizeof(resp), "HTTP/1.0 2000 OK\r\nContent-Length: %zu\r\n\r\n%s", strlen(ok), ok);
    CHECK(body_of(resp, body, sizeof(body)) == -1);
    snprintf(resp, sizeof(resp), "HTTP/1.0 500 Internal\r\nContent-Length: %zu\r\n\r\n%s", strlen(ok), ok);
    CHECK(body_of(resp, body, sizeof(body)) == -1);
    snprintf(resp, sizeof(resp), "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n%s", strlen(ok), ok);
    CHECK(body_of(resp, body, 10) == -1);                                            /* does not fit */
    CHECK(body_of("HTTP/1.0 200 OK", body, sizeof(body)) == -1);
}

static void test_bridge_must_be_loopback(void) {
#ifndef _WIN32
    cardman_auth_request_t req = { "4012888888881881", "2468", "3125551212", 101, "2125551000",
                                   "2125551000", 7, "2026-10-13T18:42:55Z", 0x55, 0 };
    cardman_auth_result_t  res;

    CHECK(cardman_bridge_addr_is_loopback("127.0.0.1:8420") == 1);
    CHECK(cardman_bridge_addr_is_loopback("127.8.9.10:8420") == 1);
    CHECK(cardman_bridge_addr_is_loopback("::1:8420") == 1);
    CHECK(cardman_bridge_addr_is_loopback("192.0.2.1:8420") == 0);       /* TEST-NET-1 */
    CHECK(cardman_bridge_addr_is_loopback("10.0.0.1:8420") == 0);
    CHECK(cardman_bridge_addr_is_loopback("2001:db8::1:8420") == 0);
    CHECK(cardman_bridge_addr_is_loopback("::ffff:192.0.2.1:8420") == 0); /* v4-mapped */
    CHECK(cardman_bridge_addr_is_loopback("8420") == 0);

    /* And the client acts on it: refused, declined, nothing sent. */
    setenv("CARDMAN_BRIDGE_ADDR", "192.0.2.1:8420", 1);
    CHECK(cardman_bridge_authorize(&req, &res) == -1);
    CHECK(res.approved == 0);
    unsetenv("CARDMAN_BRIDGE_ADDR");
#endif
}

static void test_international(void) {
    CHECK(cardman_call_type_is_international(0x56) == 1);   /* calling card, international */
    CHECK(cardman_call_type_is_international(0x06) == 1);
    CHECK(cardman_call_type_is_international(0x55) == 0);   /* inter-LATA */
    CHECK(cardman_call_type_is_international(0x51) == 0);   /* unanswered */
    CHECK(cardman_call_type_is_international(0x86) == 1);
}

static void test_cdr_without_auth_code_is_not_forwarded(void) {
    dlog_mt_call_details_t cdr;

    memset(&cdr, 0, sizeof(cdr));
    cdr.auth_code = 0;
    CHECK(cardman_forward_cdr("2125551000", &cdr) == 0);

    /* An auth_code CARDMAN could not have issued is refused locally. */
    cdr.auth_code = CARDMAN_AUTH_CODE_LIMIT;
    CHECK(cardman_forward_cdr("2125551000", &cdr) == -1);
}

int main(void) {
    /* Pin the zone so the local -> UTC conversion is deterministic. */
    set_tz("UTC");

    test_auth_code_codec();
    test_requested_at();
    test_terminal_ts();
    test_international();
    test_bridge_response();
    test_bridge_must_be_loopback();
    test_cdr_without_auth_code_is_not_forwarded();

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("mm_cardman_test: all checks passed\n");
    return 0;
}

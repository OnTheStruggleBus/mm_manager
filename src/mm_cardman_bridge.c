/*
 * CARDMAN WAPI bridge client for mm_manager. See mm_cardman_bridge.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "mm_cardman_bridge.h"

/* Skips JSON whitespace. */
static const char *json_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

/* Parses a JSON string with no escapes, returning a pointer past its
 * closing quote, or NULL. The bridge's responses never need escapes, and
 * without them no value can contain a quote, so a "key": needle can only
 * ever match a real key.
 */
static const char *json_plain_string(const char *p, const char **start, size_t *len) {
    if (*p != '"') {
        return NULL;
    }
    p++;
    *start = p;
    while (*p != '"') {
        if (*p == '\0' || *p == '\\' || (unsigned char)*p < 0x20) {
            return NULL;
        }
        p++;
    }
    *len = (size_t)(p - *start);
    return p + 1;
}

int cardman_bridge_json_flat_ok(const char *body) {
    const char *keys[CARDMAN_BRIDGE_JSON_MAX_KEYS];
    size_t      key_lens[CARDMAN_BRIDGE_JSON_MAX_KEYS];
    int         nkeys = 0;
    const char *p = json_ws(body);

    if (*p++ != '{') {
        return 0;
    }
    p = json_ws(p);
    if (*p == '}') {
        return *json_ws(p + 1) == '\0';
    }
    for (;;) {
        const char *k, *v;
        size_t      klen, vlen;

        if ((p = json_plain_string(p, &k, &klen)) == NULL || nkeys == CARDMAN_BRIDGE_JSON_MAX_KEYS) {
            return 0;
        }
        for (int i = 0; i < nkeys; i++) {
            if (key_lens[i] == klen && memcmp(keys[i], k, klen) == 0) {
                return 0;                      /* duplicate key: ambiguous */
            }
        }
        keys[nkeys] = k;
        key_lens[nkeys++] = klen;

        p = json_ws(p);
        if (*p++ != ':') {
            return 0;
        }
        p = json_ws(p);
        if (*p == '"') {
            if ((p = json_plain_string(p, &v, &vlen)) == NULL) {
                return 0;
            }
        } else if (strncmp(p, "true", 4) == 0) {
            p += 4;
        } else if (strncmp(p, "false", 5) == 0) {
            p += 5;
        } else if (strncmp(p, "null", 4) == 0) {
            p += 4;
        } else {
            const char *n = p;

            if (*p == '-') {
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                p++;
            }
            if (p == n || (*n == '-' && p == n + 1)) {
                return 0;                      /* nested value, or garbage */
            }
        }
        p = json_ws(p);
        if (*p == ',') {
            p = json_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            return *json_ws(p + 1) == '\0';
        }
        return 0;
    }
}

int cardman_bridge_http_body(const char *response, size_t response_len, char *out_body, size_t out_body_len) {
    const char *end_of_headers;
    const char *cl;
    size_t      header_len, body_len, content_length = 0;
    int         have_length = 0;

    /* Status line must be exactly "HTTP/1.x 200 ...". */
    if (response_len < 13 || strncmp(response, "HTTP/1.", 7) != 0 || strncmp(response + 8, " 200", 4) != 0 ||
        (response[12] != ' ' && response[12] != '\r')) {
        return -1;
    }
    end_of_headers = strstr(response, "\r\n\r\n");
    if (end_of_headers == NULL) {
        return -1;
    }
    header_len = (size_t)(end_of_headers - response) + 4;
    body_len = response_len - header_len;

    /* The body must be exactly Content-Length bytes: a short read is a
     * truncated response, not a smaller one.
     */
    for (cl = strstr(response, "\r\n"); cl != NULL && cl < end_of_headers; cl = strstr(cl + 2, "\r\n")) {
        const char *v = cl + 2;

        if (strncmp(v, "Content-Length:", 15) != 0 && strncmp(v, "content-length:", 15) != 0) {
            continue;
        }
        if (have_length) {
            return -1;
        }
        v += 15;
        while (*v == ' ') {
            v++;
        }
        if (*v < '0' || *v > '9') {
            return -1;
        }
        while (*v >= '0' && *v <= '9') {
            content_length = content_length * 10 + (size_t)(*v++ - '0');
            if (content_length > response_len) {
                return -1;
            }
        }
        if (*v != '\r' && *v != ' ') {
            return -1;
        }
        have_length = 1;
    }
    if (!have_length || content_length != body_len || body_len >= out_body_len) {
        return -1;
    }
    memcpy(out_body, end_of_headers + 4, body_len);
    out_body[body_len] = '\0';
    return cardman_bridge_json_flat_ok(out_body) ? 0 : -1;
}

#ifndef _WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

/* Above the bridge's own budget (CAUT has a 3 s route timeout and the
 * bridge retries once), well under the terminal's 40 s
 * card_auth_required_timeout. A client timeout shorter than the bridge's
 * would decline calls CARDMAN approved, leaving an orphaned hold.
 */
#define CARDMAN_BRIDGE_TIMEOUT_SEC 10
#define CARDMAN_BRIDGE_BUF_SIZE    2048
#define CARDMAN_BRIDGE_REQ_SIZE    1024

/* Split "host:port" (from CARDMAN_BRIDGE_ADDR or the built-in default)
 * into separate strings. Returns 0 on success.
 */
static int split_host_port(const char *addr, char *host, size_t host_len, char *port, size_t port_len) {
    const char *colon = strrchr(addr, ':');

    if (colon == NULL) {
        return -1;
    }

    size_t hlen = (size_t)(colon - addr);
    if (hlen == 0 || hlen >= host_len) {
        return -1;
    }

    memcpy(host, addr, hlen);
    host[hlen] = '\0';

    snprintf(port, port_len, "%s", colon + 1);
    return 0;
}

static int sockaddr_is_loopback(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;

        return (ntohl(in->sin_addr.s_addr) >> 24) == 127;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;

        return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
    }
    return 0;
}

int cardman_bridge_addr_is_loopback(const char *addr) {
    struct addrinfo hints = { 0 };
    struct addrinfo *res = NULL, *ai;
    char host[64];
    char port[16];
    int  ok;

    if (split_host_port(addr, host, sizeof(host), port, sizeof(port)) != 0) {
        return 0;
    }
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
        return 0;
    }
    ok = 1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        ok = ok && sockaddr_is_loopback(ai->ai_addr);
    }
    freeaddrinfo(res);
    return ok;
}

/* Returns a connected socket, -1 if it cannot connect, or -2 if the
 * address is not loopback.
 */
static int connect_with_timeout(const char *host, const char *port) {
    struct addrinfo hints = { 0 };
    struct addrinfo *res = NULL;
    int fd = -1;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    /* Card data travels in clear JSON: never to anything but this host
     * (AI/SECURITY_RULES.md rule 5), whatever CARDMAN_BRIDGE_ADDR says.
     */
    if (!sockaddr_is_loopback(res->ai_addr)) {
        freeaddrinfo(res);
        return -2;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = CARDMAN_BRIDGE_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

/* Very small helper: find "key":"value" in a JSON-ish buffer and copy
 * the value out. Not a general JSON parser - the bridge's response shape
 * is fixed and small by design. A value that does not fit is an error,
 * never truncated: a truncated auth_code would look valid.
 */
static int extract_json_string(const char *body, const char *key, char *out, size_t out_len) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);

    const char *p = strstr(body, needle);
    if (p == NULL) {
        return -1;
    }
    p += strlen(needle);
    while (*p == ' ') {
        p++;
    }
    if (*p != '"') {
        return -1;
    }
    p++;

    const char *end = strchr(p, '"');
    if (end == NULL) {
        return -1;
    }

    size_t len = (size_t)(end - p);
    if (len >= out_len) {
        return -1;
    }

    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int extract_json_bool(const char *body, const char *key, int *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);

    const char *p = strstr(body, needle);
    if (p == NULL) {
        return -1;
    }
    p += strlen(needle);
    while (*p == ' ') {
        p++;
    }

    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

/* 1 if s is non-NULL and every character is in allowed (an empty string
 * passes only when allow_empty is set). Everything placed in a JSON
 * string below goes through this, so no quote, backslash or control
 * character can reach the request body.
 */
static int chars_ok(const char *s, const char *allowed, int allow_empty) {
    if (s == NULL) {
        return 0;
    }
    if (s[0] == '\0') {
        return allow_empty;
    }
    return strspn(s, allowed) == strlen(s);
}

#define DIGITS    "0123456789"
#define AC_CHARS  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define TS_CHARS  "0123456789-:TZ"
#define ID_CHARS  "0123456789-:TZm"

/* Shared POST plumbing for /authorize and /cdr: connects to
 * CARDMAN_BRIDGE_ADDR (or the built-in default), sends request_body as
 * the JSON body of a POST to path, and copies the response body (past
 * the HTTP headers) into out_body/out_body_len. Returns 0 only for an
 * HTTP 200 response (caller still checks the JSON payload), -1 on any
 * transport failure or other status - both cases already logged with
 * the given log_prefix.
 */
static int bridge_http_post(const char *path, const char *request_body, const char *log_prefix,
                             char *out_body, size_t out_body_len) {
    char addr[128];
    char host[64];
    char port[16];
    char http_request[CARDMAN_BRIDGE_REQ_SIZE + 256];
    char response[CARDMAN_BRIDGE_BUF_SIZE] = { 0 };
    int fd;
    ssize_t total_read = 0;

    const char *env_addr = getenv("CARDMAN_BRIDGE_ADDR");
    snprintf(addr, sizeof(addr), "%s", (env_addr != NULL) ? env_addr : CARDMAN_BRIDGE_DEFAULT_ADDR);

    if (split_host_port(addr, host, sizeof(host), port, sizeof(port)) != 0) {
        fprintf(stderr, "%s: invalid CARDMAN_BRIDGE_ADDR '%s', failing closed.\n", log_prefix, addr);
        return -1;
    }

    fd = cardman_bridge_addr_is_loopback(addr) ? connect_with_timeout(host, port) : -2;
    if (fd == -2) {
        fprintf(stderr, "%s: bridge address %s is not loopback, refusing to send card data.\n", log_prefix, addr);
        return -1;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: could not reach bridge at %s, failing closed.\n", log_prefix, addr);
        return -1;
    }

    int body_len = snprintf(http_request, sizeof(http_request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, strlen(request_body), request_body);

    if (body_len < 0 || (size_t)body_len >= sizeof(http_request)) {
        close(fd);
        fprintf(stderr, "%s: request too large, failing closed.\n", log_prefix);
        return -1;
    }

    if (send(fd, http_request, (size_t)body_len, 0) != body_len) {
        close(fd);
        fprintf(stderr, "%s: send failed (%s), failing closed.\n", log_prefix, strerror(errno));
        return -1;
    }

    /* Read to EOF (the request says Connection: close). A timeout, an
     * error or a response that does not fit is a failure, never a
     * shorter response.
     */
    for (;;) {
        ssize_t n;

        if ((size_t)total_read >= sizeof(response) - 1) {
            close(fd);
            fprintf(stderr, "%s: bridge response too large, failing closed.\n", log_prefix);
            return -1;
        }
        n = recv(fd, response + total_read, sizeof(response) - 1 - (size_t)total_read, 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            close(fd);
            fprintf(stderr, "%s: no complete response from bridge (%s), failing closed.\n",
                    log_prefix, strerror(errno));
            return -1;
        }
        total_read += n;
    }
    close(fd);
    response[total_read] = '\0';

    if (memchr(response, '\0', (size_t)total_read) != NULL ||
        cardman_bridge_http_body(response, (size_t)total_read, out_body, out_body_len) != 0) {
        fprintf(stderr, "%s: bridge response not a complete HTTP 200 with a flat JSON object, failing closed.\n",
                log_prefix);
        return -1;
    }
    return 0;
}

int cardman_bridge_authorize(const cardman_auth_request_t *req, cardman_auth_result_t *result) {
    char request_body[CARDMAN_BRIDGE_REQ_SIZE];
    char response_body[CARDMAN_BRIDGE_BUF_SIZE];
    int  len;

    /* Fail closed by default; only flip to approved on a fully validated
     * bridge response below.
     */
    result->approved = 0;
    result->auth_code[0] = '\0';

    if (!chars_ok(req->pan, DIGITS, 0) || !chars_ok(req->pin, DIGITS, 0) ||
        !chars_ok(req->called_number, DIGITS, 1) || !chars_ok(req->terminal_id, DIGITS, 0) ||
        !chars_ok(req->phone_number, DIGITS, 0) || !chars_ok(req->requested_at, TS_CHARS, 0)) {
        fprintf(stderr, "cardman_bridge_authorize: request field failed validation, failing closed.\n");
        return -1;
    }

    len = snprintf(request_body, sizeof(request_body),
        "{\"pan\":\"%s\",\"pin\":\"%s\",\"service_code\":\"%u\",\"called_number\":\"%s\","
        "\"terminal_id\":\"%s\",\"phone_number\":\"%s\",\"seq_num\":%u,\"requested_at\":\"%s\","
        "\"call_type\":%u,\"international\":\"%s\"}",
        req->pan, req->pin, req->service_code, req->called_number,
        req->terminal_id, req->phone_number, req->seq_num, req->requested_at,
        req->call_type, req->international ? "yes" : "no");
    if (len < 0 || (size_t)len >= sizeof(request_body)) {
        fprintf(stderr, "cardman_bridge_authorize: request too large, failing closed.\n");
        return -1;
    }

    if (bridge_http_post("/authorize", request_body, "cardman_bridge_authorize",
                          response_body, sizeof(response_body)) != 0) {
        return -1;
    }

    int approved = 0;
    if (extract_json_bool(response_body, "approved", &approved) != 0) {
        fprintf(stderr, "cardman_bridge_authorize: no 'approved' field in response, failing closed.\n");
        return -1;
    }

    if (!approved) {
        /* A well-formed decline is not a transport failure. */
        return 0;
    }

    char auth_code[16];
    if (extract_json_string(response_body, "auth_code", auth_code, sizeof(auth_code)) != 0) {
        fprintf(stderr, "cardman_bridge_authorize: approved with no usable auth_code, failing closed.\n");
        return -1;
    }

    /* CAUT's auth_code is exactly 8 chars of [0-9A-Z] (auth.rexx
     * DERIVE_AC), not hex.
     */
    if (strlen(auth_code) != 8 || !chars_ok(auth_code, AC_CHARS, 0)) {
        fprintf(stderr, "cardman_bridge_authorize: malformed auth_code, failing closed.\n");
        return -1;
    }

    /* Length checked above; an explicit copy because GCC's
     * -Wformat-truncation can't see that check through snprintf.
     */
    memcpy(result->auth_code, auth_code, 8);
    result->auth_code[8] = '\0';
    result->approved = 1;
    return 0;
}

int cardman_bridge_submit_cdr(const cardman_cdr_request_t *req) {
    char request_body[CARDMAN_BRIDGE_REQ_SIZE];
    char response_body[CARDMAN_BRIDGE_BUF_SIZE];
    int  len;

    if (!chars_ok(req->cdr_id, ID_CHARS, 0) || !chars_ok(req->auth_code, AC_CHARS, 0) ||
        strlen(req->auth_code) != 8 || !chars_ok(req->pan, DIGITS, 0) ||
        !chars_ok(req->called_number, DIGITS, 1) || !chars_ok(req->start_ts, TS_CHARS, 0) ||
        !chars_ok(req->terminal_id, DIGITS, 0) || !chars_ok(req->phone_number, DIGITS, 0)) {
        fprintf(stderr, "cardman_bridge_submit_cdr: request field failed validation, not sent.\n");
        return -1;
    }

    len = snprintf(request_body, sizeof(request_body),
        "{\"cdr_id\":\"%s\",\"auth_code\":\"%s\",\"pan\":\"%s\",\"called_number\":\"%s\","
        "\"start_ts\":\"%s\",\"duration_secs\":%u,\"call_type\":%u,\"rate_type\":%u,"
        "\"call_cost_collected\":%u,\"call_cost_requested\":%u,"
        "\"terminal_id\":\"%s\",\"phone_number\":\"%s\",\"seq\":%u}",
        req->cdr_id, req->auth_code, req->pan, req->called_number,
        req->start_ts, req->duration_secs, req->call_type, req->rate_type,
        req->call_cost_collected, req->call_cost_requested,
        req->terminal_id, req->phone_number, req->seq);
    if (len < 0 || (size_t)len >= sizeof(request_body)) {
        fprintf(stderr, "cardman_bridge_submit_cdr: request too large, not sent.\n");
        return -1;
    }

    if (bridge_http_post("/cdr", request_body, "cardman_bridge_submit_cdr",
                          response_body, sizeof(response_body)) != 0) {
        return -1;
    }

    int delivered = 0;
    if (extract_json_bool(response_body, "delivered", &delivered) != 0) {
        fprintf(stderr, "cardman_bridge_submit_cdr: no 'delivered' field in response.\n");
        return -1;
    }
    return delivered ? 1 : 0;
}

#else /* _WIN32 */

/* Windows bridge transport not yet implemented for this Phase 1 stub -
 * fail closed rather than silently no-op approving.
 */
int cardman_bridge_authorize(const cardman_auth_request_t *req, cardman_auth_result_t *result) {
    (void)req;
    result->approved = 0;
    result->auth_code[0] = '\0';
    fprintf(stderr, "cardman_bridge_authorize: Windows transport not implemented, failing closed.\n");
    return -1;
}

int cardman_bridge_submit_cdr(const cardman_cdr_request_t *req) {
    (void)req;
    fprintf(stderr, "cardman_bridge_submit_cdr: Windows transport not implemented.\n");
    return -1;
}

#endif /* _WIN32 */

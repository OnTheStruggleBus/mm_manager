/*
 * CARDMAN WAPI bridge client for mm_manager. See mm_cardman_bridge.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "mm_cardman_bridge.h"

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

#define CARDMAN_BRIDGE_TIMEOUT_SEC 3
#define CARDMAN_BRIDGE_BUF_SIZE    2048

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

static int connect_with_timeout(const char *host, const char *port) {
    struct addrinfo hints = { 0 };
    struct addrinfo *res = NULL;
    int fd = -1;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
        return -1;
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

/* Very small helper: find "key":"value" or "key":literal in a JSON-ish
 * buffer and copy the value out. Not a general JSON parser - the Phase 1
 * stub bridge's response shape is fixed and small by design.
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
        len = out_len - 1;
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

int cardman_bridge_authorize(const cardman_auth_request_t *req, cardman_auth_result_t *result) {
    char addr[128];
    char host[64];
    char port[16];
    char request_body[512];
    char http_request[768];
    char response[CARDMAN_BRIDGE_BUF_SIZE] = { 0 };
    int fd;
    ssize_t total_read = 0;

    /* Fail closed by default; only flip to approved on a fully validated
     * bridge response below.
     */
    result->approved = 0;
    result->auth_code = 0;

    const char *env_addr = getenv("CARDMAN_BRIDGE_ADDR");
    snprintf(addr, sizeof(addr), "%s", (env_addr != NULL) ? env_addr : CARDMAN_BRIDGE_DEFAULT_ADDR);

    if (split_host_port(addr, host, sizeof(host), port, sizeof(port)) != 0) {
        fprintf(stderr, "cardman_bridge_authorize: invalid CARDMAN_BRIDGE_ADDR '%s', failing closed.\n", addr);
        return -1;
    }

    fd = connect_with_timeout(host, port);
    if (fd < 0) {
        fprintf(stderr, "cardman_bridge_authorize: could not reach bridge at %s, failing closed.\n", addr);
        return -1;
    }

    snprintf(request_body, sizeof(request_body),
        "{\"pan\":\"%s\",\"pin\":\"%s\",\"service_code\":\"%u\",\"called_number\":\"%s\",\"terminal_id\":\"%s\"}",
        req->pan, req->pin, req->service_code, req->called_number, req->terminal_id);

    int body_len = snprintf(http_request, sizeof(http_request),
        "POST /authorize HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        host, strlen(request_body), request_body);

    if (body_len < 0 || (size_t)body_len >= sizeof(http_request)) {
        close(fd);
        fprintf(stderr, "cardman_bridge_authorize: request too large, failing closed.\n");
        return -1;
    }

    if (send(fd, http_request, (size_t)body_len, 0) != body_len) {
        close(fd);
        fprintf(stderr, "cardman_bridge_authorize: send failed (%s), failing closed.\n", strerror(errno));
        return -1;
    }

    for (;;) {
        ssize_t n = recv(fd, response + total_read, sizeof(response) - 1 - (size_t)total_read, 0);
        if (n <= 0) {
            break;
        }
        total_read += n;
        if ((size_t)total_read >= sizeof(response) - 1) {
            break;
        }
    }
    close(fd);

    if (total_read <= 0) {
        fprintf(stderr, "cardman_bridge_authorize: no response from bridge, failing closed.\n");
        return -1;
    }
    response[total_read] = '\0';

    char *body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        fprintf(stderr, "cardman_bridge_authorize: malformed HTTP response, failing closed.\n");
        return -1;
    }
    body += 4;

    int approved = 0;
    if (extract_json_bool(body, "approved", &approved) != 0) {
        fprintf(stderr, "cardman_bridge_authorize: no 'approved' field in response, failing closed.\n");
        return -1;
    }

    if (!approved) {
        /* A well-formed decline is not a transport failure. */
        return 0;
    }

    char auth_code_hex[32];
    if (extract_json_string(body, "auth_code", auth_code_hex, sizeof(auth_code_hex)) != 0) {
        fprintf(stderr, "cardman_bridge_authorize: approved with no auth_code, failing closed.\n");
        return -1;
    }

    /* CAUT's auth_code is an 8-hex-char (32-bit) string; hex-decode into
     * the low 32 bits of the wire uint64_t, per INTEGRATION_PROPOSAL.md
     * section 3.
     */
    result->auth_code = (uint64_t)strtoul(auth_code_hex, NULL, 16);
    result->approved = 1;
    return 0;
}

#else /* _WIN32 */

/* Windows bridge transport not yet implemented for this Phase 1 stub -
 * fail closed rather than silently no-op approving.
 */
int cardman_bridge_authorize(const cardman_auth_request_t *req, cardman_auth_result_t *result) {
    (void)req;
    result->approved = 0;
    result->auth_code = 0;
    fprintf(stderr, "cardman_bridge_authorize: Windows transport not implemented, failing closed.\n");
    return -1;
}

#endif /* _WIN32 */

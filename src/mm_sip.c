/*
 * Minimal SIP user agent with G.711 RTP, for the virtual modem.
 *
 * This is deliberately small: UDP transport, IPv4, one call at a time.
 * It supports:
 *   - REGISTER with MD5 digest authentication and periodic refresh,
 *     including NAT discovery via the received/rport Via parameters.
 *   - Incoming INVITE (with or without an SDP offer), CANCEL, ACK, BYE,
 *     re-INVITE / UPDATE, and OPTIONS.
 *   - Outgoing BYE when mm_manager hangs up.
 *   - G.711 u-law / A-law RTP with symmetric RTP (latching) and a small
 *     re-ordering buffer.
 *
 * Lessons from fax-over-IP apply to modem calls: the SDP answer only
 * offers G.711, requests voice-band data handling (V.152 "vbd=yes"), and
 * disables silence suppression, so gateways switch off VAD, comfort noise
 * and adaptive jitter buffering for the call.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "mm_osal.h"
#include "mm_sip.h"
#include "mm_softmodem.h"

#ifdef _MSC_VER
# define strncasecmp _strnicmp
# define strcasecmp  _stricmp
# define strtok_r    strtok_s
#else
# include <strings.h>
#endif /* _MSC_VER */

#ifndef VERSION
# define VERSION "unknown"
#endif /* VERSION */

#define SIP_USER_AGENT      "mmumblephone/" VERSION " (mm_manager fork)"
#define SIP_MAX_MSG         (4096)
#define SIP_MAX_HEADERS     (64)
#define SIP_T1_MS           (500)
#define SIP_T2_MS           (4000)
#define SIP_TIMEOUT_MS      (64 * SIP_T1_MS)
#define RTP_SAMPLES         (160)
#define RTP_MAX_PAYLOAD     (480)
#define RTP_REORDER_SLOTS   (16)
#define RTP_REORDER_DEPTH   (3)     /* Packets past a gap before declaring it lost */

typedef enum {
    CALL_IDLE = 0,
    CALL_RINGING,       /* INVITE received, 180 sent */
    CALL_ANSWERED,      /* 200 OK sent, waiting for ACK */
    CALL_CONFIRMED      /* ACK received */
} call_state_t;

typedef struct sip_header {
    char  name[32];
    char *value;
} sip_header_t;

typedef struct sip_msg {
    int          is_request;
    char         method[16];
    char         uri[512];
    int          status;
    int          nhdr;
    sip_header_t hdr[SIP_MAX_HEADERS];
    char        *body;
    int          body_len;
    char         buf[SIP_MAX_MSG + 1];
} sip_msg_t;

/* Non-INVITE client transaction (REGISTER, BYE) */
typedef struct sip_client_tx {
    int                active;
    char               method[16];
    char               call_id[128];
    uint32_t           cseq;
    char               msg[SIP_MAX_MSG];
    int                len;
    struct sockaddr_in dest;
    uint64_t           next_retx;
    uint32_t           interval;
    uint64_t           deadline;
    int                auth_tries;
} sip_client_tx_t;

typedef struct rtp_slot {
    int      used;
    uint16_t seq;
    int      len;
    uint8_t  payload[RTP_MAX_PAYLOAD];
} rtp_slot_t;

struct sip_ua {
    sip_config_t       cfg;
    sip_callbacks_t    cb;
    void              *user;

    osal_socket_t      sip_sock;
    osal_socket_t      rtp_sock;
    uint16_t           rtp_port;
    char               contact_ip[64];
    uint16_t           contact_port;
    struct sockaddr_in registrar_addr;
    struct in_addr     bind_addr;           /* Address the SIP and RTP sockets are bound to */
    struct in_addr     allow_addr;          /* Only peer accepted, when allow_set */
    int                allow_set;
    uint32_t           dropped_packets;     /* SIP/RTP packets dropped from other addresses */

    /* Registration */
    int                registered;
    int                nat_checked;
    char               reg_call_id[128];
    char               reg_from_tag[32];
    uint32_t           reg_cseq;
    uint64_t           next_register;
    sip_client_tx_t    reg_tx;

    /* Call */
    call_state_t       call_state;
    char               call_id[256];
    char               local_tag[32];
    char               remote_from[512];
    char               local_to[512];
    char               invite_vias[1536];
    char               route_set[1536];     /* Route headers for in-dialog requests */
    char               record_route[1536];  /* Record-Route headers echoed in 2xx */
    char               remote_target[512];
    char               invite_cseq[64];
    uint32_t           local_cseq;
    struct sockaddr_in sig_addr;
    int                answer_requested;
    int                sdp_offer_pending;  /* INVITE had no SDP, our offer is in the 200 OK */
    char               last_response[SIP_MAX_MSG];
    int                last_response_len;
    uint64_t           resp_next_retx;
    uint32_t           resp_interval;
    uint64_t           resp_deadline;
    uint32_t           sdp_version;
    sip_client_tx_t    bye_tx;

    /* Media */
    int                media_active;
    int                payload_type;
    struct sockaddr_in rtp_remote;
    int                rtp_latched;
    uint16_t           tx_seq;
    uint32_t           tx_ts;
    uint32_t           tx_ssrc;
    int                tx_marker;
    int                rx_started;
    uint16_t           rx_expected;
    int                rx_last_len;
    rtp_slot_t         rx_slots[RTP_REORDER_SLOTS];
};

static void sip_send_register(sip_ua_t *ua, const char *auth_hdr);
static void end_call(sip_ua_t *ua, const char *reason, int send_bye);

/*
 * MD5 (RFC 1321), used for digest authentication.
 */
typedef struct md5_ctx {
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} md5_ctx_t;

#define MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const int r[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    uint32_t w[16];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
    }

    for (i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        uint32_t tmp;

        if (i < 16) {
            f = MD5_F(b, c, d);
            g = i;
        } else if (i < 32) {
            f = MD5_G(b, c, d);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = MD5_H(b, c, d);
            g = (3 * i + 5) % 16;
        } else {
            f = MD5_I(b, c, d);
            g = (7 * i) % 16;
        }

        tmp = d;
        d = c;
        c = b;
        b = b + MD5_ROTL(a + f + k[i] + w[g], r[i]);
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(md5_ctx_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->count = 0;
}

static void md5_update(md5_ctx_t *ctx, const uint8_t *data, size_t len) {
    size_t i;

    for (i = 0; i < len; i++) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if ((ctx->count % 64) == 0) {
            md5_transform(ctx->state, ctx->buffer);
        }
    }
}

static void md5_final(md5_ctx_t *ctx, uint8_t digest[16]) {
    uint64_t bits = ctx->count * 8;
    uint8_t pad = 0x80;
    uint8_t zero = 0;
    uint8_t len_bytes[8];
    int i;

    md5_update(ctx, &pad, 1);
    while ((ctx->count % 64) != 56) {
        md5_update(ctx, &zero, 1);
    }

    for (i = 0; i < 8; i++) {
        len_bytes[i] = (uint8_t)(bits >> (8 * i));
    }
    md5_update(ctx, len_bytes, 8);

    for (i = 0; i < 16; i++) {
        digest[i] = (uint8_t)(ctx->state[i / 4] >> (8 * (i % 4)));
    }
}

/* MD5 of a string, as 32 lower-case hex digits. */
void sip_md5_hex(const char *str, char out[33]) {
    md5_ctx_t ctx;
    uint8_t digest[16];
    int i;

    md5_init(&ctx);
    md5_update(&ctx, (const uint8_t *)str, strlen(str));
    md5_final(&ctx, digest);

    for (i = 0; i < 16; i++) {
        snprintf(&out[i * 2], 3, "%02x", digest[i]);
    }
}

/*
 * Utilities
 */

/* Copy a string, truncating if necessary. */
static void copy_str(char *dst, size_t size, const char *src) {
    size_t n = (src != NULL) ? strlen(src) : 0;

    if (size == 0) return;
    if (n >= size) n = size - 1;
    if (n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

static void random_hex(char *dst, size_t size, int digits) {
    int i;

    if ((size_t)digits >= size) digits = (int)size - 1;
    for (i = 0; i < digits; i++) {
        dst[i] = "0123456789abcdef"[osal_random32() & 0xF];
    }
    dst[digits] = '\0';
}

static int resolve_ipv4(const char *host, uint16_t port, struct sockaddr_in *addr) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if ((getaddrinfo(host, NULL, &hints, &res) != 0) || (res == NULL)) {
        return -1;
    }

    memcpy(addr, res->ai_addr, sizeof(*addr));
    addr->sin_port = htons(port);
    freeaddrinfo(res);
    return 0;
}

static void addr_to_str(const struct sockaddr_in *addr, char *buf, size_t size) {
    const uint8_t *ip = (const uint8_t *)&addr->sin_addr;

    snprintf(buf, size, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(addr->sin_port));
}

/* Find the local IP address used to reach dest (no packets are sent.) */
static int local_ip_for(const struct sockaddr_in *dest, char *buf, size_t size) {
    osal_socket_t s;
    struct sockaddr_in local;
    osal_socklen_t len = (osal_socklen_t)sizeof(local);
    const uint8_t *ip;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == OSAL_INVALID_SOCKET) return -1;

    if ((connect(s, (const struct sockaddr *)dest, (osal_socklen_t)sizeof(*dest)) != 0) ||
        (getsockname(s, (struct sockaddr *)&local, &len) != 0)) {
        osal_close_socket(s);
        return -1;
    }
    osal_close_socket(s);

    ip = (const uint8_t *)&local.sin_addr;
    snprintf(buf, size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return 0;
}

static osal_socket_t open_udp(struct in_addr bind_addr, uint16_t port) {
    osal_socket_t s;
    struct sockaddr_in addr;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == OSAL_INVALID_SOCKET) return s;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr = bind_addr;
    addr.sin_port = htons(port);

    if (bind(s, (const struct sockaddr *)&addr, (osal_socklen_t)sizeof(addr)) != 0) {
        osal_close_socket(s);
        return OSAL_INVALID_SOCKET;
    }

#ifdef _WIN32
    {
        /* Don't let ICMP port unreachable errors make recvfrom() fail on Windows. */
# ifndef SIO_UDP_CONNRESET
#  define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
# endif
        BOOL new_behavior = FALSE;
        DWORD bytes_returned = 0;

        WSAIoctl(s, SIO_UDP_CONNRESET, &new_behavior, (DWORD)sizeof(new_behavior), NULL, 0, &bytes_returned, NULL, NULL);
    }
#endif /* _WIN32 */

    return s;
}

static void udp_send(sip_ua_t *ua, osal_socket_t s, const struct sockaddr_in *dest, const void *buf, int len) {
    if (sendto(s, (const char *)buf, len, 0, (const struct sockaddr *)dest, (osal_socklen_t)sizeof(*dest)) < 0) {
        if (ua->cfg.debug) fprintf(stderr, "SIP: sendto() failed.\n");
    }
}

static void sip_send(sip_ua_t *ua, const struct sockaddr_in *dest, const char *msg, int len) {
    if (ua->cfg.debug > 1) {
        char addr[32];

        addr_to_str(dest, addr, sizeof(addr));
        printf("SIP: >>> %s\n%.*s\n", addr, len, msg);
    }
    udp_send(ua, ua->sip_sock, dest, msg, len);
}

/*
 * SIP message parsing
 */

static const char *compact_header(const char *name) {
    if (strlen(name) != 1) return name;
    switch (tolower((unsigned char)name[0])) {
    case 'v': return "Via";
    case 'f': return "From";
    case 't': return "To";
    case 'i': return "Call-ID";
    case 'm': return "Contact";
    case 'l': return "Content-Length";
    case 'c': return "Content-Type";
    default: return name;
    }
}

static int sip_parse(sip_msg_t *msg, const char *data, int len) {
    char *p;
    char *line;
    char *end;
    char *hdr_end;

    memset(msg, 0, offsetof(sip_msg_t, buf));
    if (len > SIP_MAX_MSG) return -1;
    memcpy(msg->buf, data, (size_t)len);
    msg->buf[len] = '\0';

    /* Separate headers from body. */
    hdr_end = strstr(msg->buf, "\r\n\r\n");
    if (hdr_end != NULL) {
        *hdr_end = '\0';
        msg->body = hdr_end + 4;
    } else if ((hdr_end = strstr(msg->buf, "\n\n")) != NULL) {
        *hdr_end = '\0';
        msg->body = hdr_end + 2;
    } else {
        msg->body = msg->buf + len;
    }
    msg->body_len = (int)strlen(msg->body);

    /* Start line */
    line = msg->buf;
    end = strchr(line, '\n');
    if (end != NULL) *end = '\0';
    if ((p = strchr(line, '\r')) != NULL) *p = '\0';

    if (strncmp(line, "SIP/2.0 ", 8) == 0) {
        msg->is_request = 0;
        msg->status = atoi(line + 8);
    } else {
        char version[16];

        msg->is_request = 1;
        if (sscanf(line, "%15s %511s %15s", msg->method, msg->uri, version) != 3) {
            return -1;
        }
    }

    /* Headers */
    while ((end != NULL) && (msg->nhdr < SIP_MAX_HEADERS)) {
        char *colon;
        char *name_end;

        line = end + 1;
        end = strchr(line, '\n');
        if (end != NULL) *end = '\0';
        if ((p = strchr(line, '\r')) != NULL) *p = '\0';

        if (*line == '\0') continue;

        /* Continuation line: append to previous header value. */
        if (((*line == ' ') || (*line == '\t')) && (msg->nhdr > 0)) {
            char *prev_end = msg->hdr[msg->nhdr - 1].value + strlen(msg->hdr[msg->nhdr - 1].value);
            while ((*line == ' ') || (*line == '\t')) line++;
            *prev_end++ = ' ';
            memmove(prev_end, line, strlen(line) + 1);
            continue;
        }

        colon = strchr(line, ':');
        if (colon == NULL) continue;

        name_end = colon;
        while ((name_end > line) && isspace((unsigned char)name_end[-1])) name_end--;
        *name_end = '\0';
        copy_str(msg->hdr[msg->nhdr].name, sizeof(msg->hdr[0].name), compact_header(line));

        p = colon + 1;
        while (isspace((unsigned char)*p)) p++;
        msg->hdr[msg->nhdr].value = p;
        msg->nhdr++;
    }

    return 0;
}

/* Return the n-th occurrence of a header (0-based), or NULL. */
static const char *sip_header(const sip_msg_t *msg, const char *name, int n) {
    int i;

    for (i = 0; i < msg->nhdr; i++) {
        if (strcasecmp(msg->hdr[i].name, name) == 0) {
            if (n-- == 0) return msg->hdr[i].value;
        }
    }
    return NULL;
}

/* Append all occurrences of a header as "Name: value\r\n" lines. */
static void sip_copy_headers(const sip_msg_t *msg, const char *name, const char *out_name, char *out, size_t size) {
    int i;
    size_t used = strlen(out);

    for (i = 0; i < msg->nhdr; i++) {
        if (strcasecmp(msg->hdr[i].name, name) == 0) {
            int n = snprintf(out + used, size - used, "%s: %s\r\n", out_name, msg->hdr[i].value);
            if ((n < 0) || ((size_t)n >= size - used)) break;
            used += (size_t)n;
        }
    }
}

/* Get the value of a ;param= from a header value. */
static int header_param(const char *value, const char *param, char *out, size_t size) {
    const char *p = value;
    size_t plen = strlen(param);

    out[0] = '\0';
    if (value == NULL) return -1;

    while ((p = strchr(p, ';')) != NULL) {
        p++;
        while (*p == ' ') p++;
        if ((strncasecmp(p, param, plen) == 0) && ((p[plen] == '=') || (p[plen] == ';') || (p[plen] == '\0') || (p[plen] == ','))) {
            size_t n = 0;

            if (p[plen] != '=') return 0;
            p += plen + 1;
            if (*p == '"') p++;
            while (p[n] && (p[n] != ';') && (p[n] != ',') && (p[n] != '"') && (p[n] != '>') && !isspace((unsigned char)p[n])) n++;
            if (n >= size) n = size - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return 0;
        }
    }
    return -1;
}

/* Extract URI from a name-addr (From, To, Contact.) */
static void header_uri(const char *value, char *out, size_t size) {
    const char *start;
    size_t n = 0;

    out[0] = '\0';
    if (value == NULL) return;

    start = strchr(value, '<');
    if (start != NULL) {
        start++;
        while (start[n] && (start[n] != '>')) n++;
    } else {
        start = value;
        while (isspace((unsigned char)*start)) start++;
        while (start[n] && (start[n] != ';') && !isspace((unsigned char)start[n])) n++;
    }

    if (n >= size) n = size - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

static int cseq_number(const sip_msg_t *msg, char *method, size_t size) {
    const char *cseq = sip_header(msg, "CSeq", 0);
    unsigned long num = 0;
    char m[32] = "";

    if ((cseq == NULL) || (sscanf(cseq, "%lu %31s", &num, m) < 1)) return -1;
    if (method != NULL) copy_str(method, size, m);
    return (int)num;
}

/* Parse a Digest challenge parameter. */
static void auth_param(const char *challenge, const char *name, char *out, size_t size) {
    const char *p = challenge;
    size_t nlen = strlen(name);

    out[0] = '\0';
    while (p && *p) {
        while (*p && (isspace((unsigned char)*p) || (*p == ','))) p++;
        if ((strncasecmp(p, name, nlen) == 0) && (p[nlen] == '=')) {
            size_t n = 0;
            int quoted;

            p += nlen + 1;
            quoted = (*p == '"');
            if (quoted) p++;
            while (p[n] && (quoted ? (p[n] != '"') : ((p[n] != ',') && !isspace((unsigned char)p[n])))) n++;
            if (n >= size) n = size - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return;
        }
        /* Skip to the next parameter. */
        while (*p && (*p != ',')) {
            if (*p == '"') {
                p++;
                while (*p && (*p != '"')) p++;
            }
            if (*p) p++;
        }
    }
}

/* Build an Authorization / Proxy-Authorization header line from a 401/407 response. */
static int build_auth_header(sip_ua_t *ua, const sip_msg_t *resp, const char *method, const char *uri,
                             char *out, size_t size) {
    const char *challenge;
    const char *hdr_name;
    char realm[128], nonce[256], opaque[256], qop[64], algorithm[32];
    char ha1_in[512], ha1[33], ha2_in[640], ha2[33], resp_in[1024], response[33];
    char cnonce[17];
    const char *user = ua->cfg.auth_user[0] ? ua->cfg.auth_user : ua->cfg.user;

    if (resp->status == 407) {
        challenge = sip_header(resp, "Proxy-Authenticate", 0);
        hdr_name = "Proxy-Authorization";
    } else {
        challenge = sip_header(resp, "WWW-Authenticate", 0);
        hdr_name = "Authorization";
    }

    if ((challenge == NULL) || (strncasecmp(challenge, "Digest", 6) != 0)) {
        return -1;
    }
    challenge += 6;

    auth_param(challenge, "realm", realm, sizeof(realm));
    auth_param(challenge, "nonce", nonce, sizeof(nonce));
    auth_param(challenge, "opaque", opaque, sizeof(opaque));
    auth_param(challenge, "qop", qop, sizeof(qop));
    auth_param(challenge, "algorithm", algorithm, sizeof(algorithm));

    if (algorithm[0] && (strcasecmp(algorithm, "MD5") != 0)) {
        fprintf(stderr, "SIP: Unsupported digest algorithm '%s'.\n", algorithm);
        return -1;
    }

    snprintf(ha1_in, sizeof(ha1_in), "%s:%s:%s", user, realm, ua->cfg.password);
    sip_md5_hex(ha1_in, ha1);
    snprintf(ha2_in, sizeof(ha2_in), "%s:%s", method, uri);
    sip_md5_hex(ha2_in, ha2);

    if (qop[0]) {
        /* qop may be a list, e.g. "auth,auth-int": only "auth" is supported. */
        random_hex(cnonce, sizeof(cnonce), 16);
        snprintf(resp_in, sizeof(resp_in), "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
        sip_md5_hex(resp_in, response);
        snprintf(out, size,
                 "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", "
                 "algorithm=MD5, qop=auth, nc=00000001, cnonce=\"%s\"%s%s%s\r\n",
                 hdr_name, user, realm, nonce, uri, response, cnonce,
                 opaque[0] ? ", opaque=\"" : "", opaque, opaque[0] ? "\"" : "");
    } else {
        snprintf(resp_in, sizeof(resp_in), "%s:%s:%s", ha1, nonce, ha2);
        sip_md5_hex(resp_in, response);
        snprintf(out, size,
                 "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", "
                 "algorithm=MD5%s%s%s\r\n",
                 hdr_name, user, realm, nonce, uri, response,
                 opaque[0] ? ", opaque=\"" : "", opaque, opaque[0] ? "\"" : "");
    }

    return 0;
}

/*
 * Configuration
 */

static void config_defaults(sip_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->registrar_port = SIP_DEFAULT_PORT;
    cfg->local_port = SIP_DEFAULT_PORT;
    cfg->rtp_port = SIP_DEFAULT_RTP_PORT;
    cfg->expires = SIP_DEFAULT_EXPIRES;
    cfg->codec = RTP_PT_PCMU;
    cfg->answer_tone = SM_ANSWER_TONE_BELL;
    copy_str(cfg->bind_ip, sizeof(cfg->bind_ip), "127.0.0.1");
    copy_str(cfg->user, sizeof(cfg->user), "mm_manager");
}

static int config_option(sip_config_t *cfg, const char *name, const char *value) {
    if (strcasecmp(name, "port") == 0) {
        cfg->local_port = (uint16_t)atoi(value);
    } else if (strcasecmp(name, "rtp_port") == 0) {
        cfg->rtp_port = (uint16_t)atoi(value);
    } else if (strcasecmp(name, "expires") == 0) {
        cfg->expires = atoi(value);
    } else if (strcasecmp(name, "auth_user") == 0) {
        copy_str(cfg->auth_user, sizeof(cfg->auth_user), value);
    } else if (strcasecmp(name, "domain") == 0) {
        copy_str(cfg->domain, sizeof(cfg->domain), value);
    } else if (strcasecmp(name, "bind") == 0) {
        copy_str(cfg->bind_ip, sizeof(cfg->bind_ip), value);
    } else if (strcasecmp(name, "allow") == 0) {
        copy_str(cfg->allow_ip, sizeof(cfg->allow_ip), value);
    } else if (strcasecmp(name, "local_ip") == 0) {
        copy_str(cfg->local_ip, sizeof(cfg->local_ip), value);
    } else if (strcasecmp(name, "password") == 0) {
        copy_str(cfg->password, sizeof(cfg->password), value);
    } else if (strcasecmp(name, "codec") == 0) {
        if (strcasecmp(value, "pcmu") == 0 || strcasecmp(value, "ulaw") == 0) {
            cfg->codec = RTP_PT_PCMU;
        } else if (strcasecmp(value, "pcma") == 0 || strcasecmp(value, "alaw") == 0) {
            cfg->codec = RTP_PT_PCMA;
        } else {
            fprintf(stderr, "SIP: Unknown codec '%s', use pcmu or pcma.\n", value);
            return -1;
        }
    } else if (strcasecmp(name, "answer_tone") == 0) {
        cfg->answer_tone = atoi(value);
        if ((cfg->answer_tone != SM_ANSWER_TONE_BELL) && (cfg->answer_tone != SM_ANSWER_TONE_V25)) {
            fprintf(stderr, "SIP: answer_tone must be %d or %d.\n", SM_ANSWER_TONE_BELL, SM_ANSWER_TONE_V25);
            return -1;
        }
    } else {
        fprintf(stderr, "SIP: Unknown option '%s'.\n", name);
        return -1;
    }
    return 0;
}

int sip_config_parse(sip_config_t *cfg, const char *spec) {
    char buf[512];
    char *opts;
    char *at;
    char *p;

    config_defaults(cfg);
    copy_str(buf, sizeof(buf), spec);

    opts = strchr(buf, ';');
    if (opts != NULL) *opts++ = '\0';

    p = buf;
    if (strncasecmp(p, "sip:", 4) == 0) p += 4;

    if ((*p != '\0') && (strcasecmp(p, "listen") != 0)) {
        char *colon;

        at = strrchr(p, '@');
        if (at == NULL) {
            fprintf(stderr, "SIP: expected user[:password]@registrar[:port] or 'listen'.\n");
            return -1;
        }
        *at = '\0';

        /* user[:password] */
        colon = strchr(p, ':');
        if (colon != NULL) {
            *colon = '\0';
            copy_str(cfg->password, sizeof(cfg->password), colon + 1);
        }
        copy_str(cfg->user, sizeof(cfg->user), p);

        /* registrar[:port] */
        p = at + 1;
        colon = strchr(p, ':');
        if (colon != NULL) {
            *colon = '\0';
            cfg->registrar_port = (uint16_t)atoi(colon + 1);
        }
        copy_str(cfg->registrar, sizeof(cfg->registrar), p);

        if ((cfg->user[0] == '\0') || (cfg->registrar[0] == '\0')) {
            fprintf(stderr, "SIP: user and registrar must not be empty.\n");
            return -1;
        }
    }

    while ((opts != NULL) && (*opts != '\0')) {
        char *next = strchr(opts, ';');
        char *eq;

        if (next != NULL) *next++ = '\0';
        eq = strchr(opts, '=');
        if (eq == NULL) {
            fprintf(stderr, "SIP: option '%s' requires a value.\n", opts);
            return -1;
        }
        *eq = '\0';
        if (config_option(cfg, opts, eq + 1) != 0) return -1;
        opts = next;
    }

    /* Allow the password to be supplied from the environment, to keep it off the command line. */
    if ((cfg->password[0] == '\0') && (getenv("MM_SIP_PASSWORD") != NULL)) {
        copy_str(cfg->password, sizeof(cfg->password), getenv("MM_SIP_PASSWORD"));
    }

    if (cfg->domain[0] == '\0') {
        copy_str(cfg->domain, sizeof(cfg->domain), cfg->registrar);
    }

    if (cfg->expires < 60) cfg->expires = 60;

    /* Card data crosses this line: refuse to listen on every interface. */
    {
        struct in_addr a;

        if ((inet_pton(AF_INET, cfg->bind_ip, &a) != 1) || (a.s_addr == htonl(INADDR_ANY))) {
            fprintf(stderr, "SIP: bind must be a specific IPv4 address (not 0.0.0.0).\n");
            return -1;
        }
        if ((cfg->allow_ip[0] != '\0') && (inet_pton(AF_INET, cfg->allow_ip, &a) != 1)) {
            fprintf(stderr, "SIP: allow must be an IPv4 address.\n");
            return -1;
        }
        inet_pton(AF_INET, cfg->bind_ip, &a);
        if ((cfg->registrar[0] == '\0') && (cfg->allow_ip[0] == '\0') &&
            ((ntohl(a.s_addr) >> 24) != 127)) {
            fprintf(stderr, "SIP: 'listen' on a non-loopback address requires allow=<PBX address>.\n");
            return -1;
        }
    }

    if (cfg->local_ip[0] == '\0') {
        copy_str(cfg->local_ip, sizeof(cfg->local_ip), cfg->bind_ip);
    }

    return 0;
}

/*
 * Client transactions (REGISTER, BYE)
 */

static void client_tx_start(sip_client_tx_t *tx, const char *method, const char *call_id, uint32_t cseq,
                            const char *msg, int len, const struct sockaddr_in *dest) {
    uint64_t now = osal_time_ms();

    tx->active = 1;
    copy_str(tx->method, sizeof(tx->method), method);
    copy_str(tx->call_id, sizeof(tx->call_id), call_id);
    tx->cseq = cseq;
    if (len > (int)sizeof(tx->msg)) len = (int)sizeof(tx->msg);
    memcpy(tx->msg, msg, (size_t)len);
    tx->len = len;
    tx->dest = *dest;
    tx->interval = SIP_T1_MS;
    tx->next_retx = now + tx->interval;
    tx->deadline = now + SIP_TIMEOUT_MS;
}

/* Returns 1 if the transaction timed out. */
static int client_tx_timer(sip_ua_t *ua, sip_client_tx_t *tx, uint64_t now) {
    if (!tx->active) return 0;

    if (now >= tx->deadline) {
        tx->active = 0;
        return 1;
    }

    if (now >= tx->next_retx) {
        sip_send(ua, &tx->dest, tx->msg, tx->len);
        tx->interval *= 2;
        if (tx->interval > SIP_T2_MS) tx->interval = SIP_T2_MS;
        tx->next_retx = now + tx->interval;
    }
    return 0;
}

/*
 * Registration
 */

static void sip_send_register(sip_ua_t *ua, const char *auth_hdr) {
    char msg[SIP_MAX_MSG];
    char uri[300];
    char branch[17];
    int len;

    snprintf(uri, sizeof(uri), "sip:%s", ua->cfg.domain);
    random_hex(branch, sizeof(branch), 16);
    ua->reg_cseq++;

    len = snprintf(msg, sizeof(msg),
                   "REGISTER %s SIP/2.0\r\n"
                   "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK%s;rport\r\n"
                   "Max-Forwards: 70\r\n"
                   "From: <sip:%s@%s>;tag=%s\r\n"
                   "To: <sip:%s@%s>\r\n"
                   "Call-ID: %s\r\n"
                   "CSeq: %u REGISTER\r\n"
                   "Contact: <sip:%s@%s:%u>\r\n"
                   "Expires: %d\r\n"
                   "%s"
                   "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, UPDATE\r\n"
                   "User-Agent: " SIP_USER_AGENT "\r\n"
                   "Content-Length: 0\r\n\r\n",
                   uri,
                   ua->contact_ip, ua->contact_port, branch,
                   ua->cfg.user, ua->cfg.domain, ua->reg_from_tag,
                   ua->cfg.user, ua->cfg.domain,
                   ua->reg_call_id,
                   ua->reg_cseq,
                   ua->cfg.user, ua->contact_ip, ua->contact_port,
                   ua->cfg.expires,
                   auth_hdr ? auth_hdr : "");

    if (len >= (int)sizeof(msg)) len = (int)sizeof(msg) - 1;

    sip_send(ua, &ua->registrar_addr, msg, len);
    client_tx_start(&ua->reg_tx, "REGISTER", ua->reg_call_id, ua->reg_cseq, msg, len, &ua->registrar_addr);
}

static void handle_register_response(sip_ua_t *ua, const sip_msg_t *msg) {
    uint64_t now = osal_time_ms();

    if (msg->status < 200) return;

    ua->reg_tx.active = 0;

    if ((msg->status == 401) || (msg->status == 407)) {
        char auth[1024];
        char uri[300];

        snprintf(uri, sizeof(uri), "sip:%s", ua->cfg.domain);
        if ((ua->reg_tx.auth_tries++ < 2) &&
            (build_auth_header(ua, msg, "REGISTER", uri, auth, sizeof(auth)) == 0)) {
            sip_send_register(ua, auth);
            return;
        }
        fprintf(stderr, "SIP: Registration failed: authentication rejected (%d).\n", msg->status);
        ua->registered = 0;
        ua->next_register = now + 60000;
        return;
    }

    if ((msg->status >= 200) && (msg->status < 300)) {
        int expires = ua->cfg.expires;
        const char *exp_hdr = sip_header(msg, "Expires", 0);
        const char *contact = sip_header(msg, "Contact", 0);
        char param[64];

        if ((contact != NULL) && (header_param(contact, "expires", param, sizeof(param)) == 0) && param[0]) {
            expires = atoi(param);
        } else if (exp_hdr != NULL) {
            expires = atoi(exp_hdr);
        }
        if (expires < 30) expires = 30;

        /* NAT discovery: if the registrar saw a different address, re-register with it. */
        if (!ua->nat_checked && (ua->cfg.local_ip[0] == '\0')) {
            const char *via = sip_header(msg, "Via", 0);
            char received[64];
            char rport[16];

            ua->nat_checked = 1;
            header_param(via, "received", received, sizeof(received));
            header_param(via, "rport", rport, sizeof(rport));

            if ((received[0] && (strcmp(received, ua->contact_ip) != 0)) ||
                (rport[0] && (atoi(rport) != ua->contact_port))) {
                if (received[0]) copy_str(ua->contact_ip, sizeof(ua->contact_ip), received);
                if (rport[0]) ua->contact_port = (uint16_t)atoi(rport);
                printf("SIP: NAT detected, using public address %s:%u.\n", ua->contact_ip, ua->contact_port);
                ua->reg_tx.auth_tries = 0;
                sip_send_register(ua, NULL);
                return;
            }
        }

        if (!ua->registered) {
            printf("SIP: Registered as %s@%s (expires %d s).\n", ua->cfg.user, ua->cfg.domain, expires);
        }
        ua->registered = 1;
        ua->reg_tx.auth_tries = 0;
        ua->next_register = now + (uint64_t)expires * 800u;   /* Refresh at 80% of expiry */
        return;
    }

    fprintf(stderr, "SIP: Registration failed with status %d, retrying in 60 seconds.\n", msg->status);
    ua->registered = 0;
    ua->reg_tx.auth_tries = 0;
    ua->next_register = now + 60000;
}

/*
 * Server side (incoming requests)
 */

/* Build and send a response to a request, returns the length. */
static int sip_respond(sip_ua_t *ua, const sip_msg_t *req, const struct sockaddr_in *src, int status,
                       const char *reason, const char *to_tag, const char *extra_headers,
                       const char *content_type, const char *body, char *save, size_t save_size) {
    char msg[SIP_MAX_MSG];
    char vias[1536] = "";
    const char *to = sip_header(req, "To", 0);
    const char *from = sip_header(req, "From", 0);
    const char *call_id = sip_header(req, "Call-ID", 0);
    const char *cseq = sip_header(req, "CSeq", 0);
    char tag_param[64] = "";
    char existing_tag[64];
    int body_len = body ? (int)strlen(body) : 0;
    int len;

    sip_copy_headers(req, "Via", "Via", vias, sizeof(vias));

    if ((to_tag != NULL) && (to_tag[0] != '\0') &&
        ((to == NULL) || (header_param(to, "tag", existing_tag, sizeof(existing_tag)) != 0))) {
        snprintf(tag_param, sizeof(tag_param), ";tag=%s", to_tag);
    }

    len = snprintf(msg, sizeof(msg),
                   "SIP/2.0 %d %s\r\n"
                   "%s"
                   "From: %s\r\n"
                   "To: %s%s\r\n"
                   "Call-ID: %s\r\n"
                   "CSeq: %s\r\n"
                   "%s"
                   "User-Agent: " SIP_USER_AGENT "\r\n"
                   "%s%s%s"
                   "Content-Length: %d\r\n\r\n"
                   "%s",
                   status, reason,
                   vias,
                   from ? from : "",
                   to ? to : "", tag_param,
                   call_id ? call_id : "",
                   cseq ? cseq : "",
                   extra_headers ? extra_headers : "",
                   content_type ? "Content-Type: " : "", content_type ? content_type : "", content_type ? "\r\n" : "",
                   body_len,
                   body ? body : "");

    if (len >= (int)sizeof(msg)) len = (int)sizeof(msg) - 1;

    sip_send(ua, src, msg, len);

    if (save != NULL) {
        if (len > (int)save_size) len = (int)save_size;
        memcpy(save, msg, (size_t)len);
    }
    return len;
}

static void build_sdp(sip_ua_t *ua, char *out, size_t size) {
    const char *codec = (ua->payload_type == RTP_PT_PCMA) ? "PCMA" : "PCMU";

    ua->sdp_version++;
    snprintf(out, size,
             "v=0\r\n"
             "o=mmumblephone %u %u IN IP4 %s\r\n"
             "s=mmumblephone\r\n"
             "c=IN IP4 %s\r\n"
             "t=0 0\r\n"
             "m=audio %u RTP/AVP %d\r\n"
             "a=rtpmap:%d %s/8000\r\n"
             "a=ptime:20\r\n"
             "a=silenceSupp:off - - - -\r\n"
             "a=gpmd:%d vbd=yes\r\n"
             "a=sendrecv\r\n",
             ua->tx_ssrc, ua->sdp_version, ua->contact_ip,
             ua->contact_ip,
             ua->rtp_port, ua->payload_type,
             ua->payload_type, codec,
             ua->payload_type);
}

/*
 * Parse an SDP offer or answer: remote RTP address and codec.
 * Returns 0 on success, -1 if no usable G.711 audio stream was found.
 */
static int parse_sdp(sip_ua_t *ua, const char *sdp) {
    char session_ip[64] = "";
    char media_ip[64] = "";
    int port = -1;
    int have_pcmu = 0;
    int have_pcma = 0;
    int in_audio = 0;
    const char *line = sdp;

    while ((line != NULL) && (*line != '\0')) {
        char buf[256];
        size_t n = strcspn(line, "\r\n");

        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, line, n);
        buf[n] = '\0';

        if (strncmp(buf, "m=", 2) == 0) {
            in_audio = 0;
            if ((port < 0) && (strncmp(buf, "m=audio ", 8) == 0)) {
                char *tok;
                char *save = NULL;
                int i = 0;

                in_audio = 1;
                tok = strtok_r(buf + 2, " ", &save);
                while (tok != NULL) {
                    if (i == 1) {
                        port = atoi(tok);
                    } else if (i >= 3) {
                        if (strcmp(tok, "0") == 0) have_pcmu = 1;
                        if (strcmp(tok, "8") == 0) have_pcma = 1;
                    }
                    tok = strtok_r(NULL, " ", &save);
                    i++;
                }
            }
        } else if (strncmp(buf, "c=IN IP4 ", 9) == 0) {
            char *ip = buf + 9;

            ip[strcspn(ip, " /")] = '\0';
            if (in_audio) {
                copy_str(media_ip, sizeof(media_ip), ip);
            } else if (port < 0) {
                copy_str(session_ip, sizeof(session_ip), ip);
            }
        }

        line += strcspn(line, "\r\n");
        while ((*line == '\r') || (*line == '\n')) line++;
    }

    if ((port <= 0) || (!have_pcmu && !have_pcma)) {
        return -1;
    }

    if (ua->cfg.codec == RTP_PT_PCMA) {
        ua->payload_type = have_pcma ? RTP_PT_PCMA : RTP_PT_PCMU;
    } else {
        ua->payload_type = have_pcmu ? RTP_PT_PCMU : RTP_PT_PCMA;
    }

    if (media_ip[0] == '\0') copy_str(media_ip, sizeof(media_ip), session_ip);

    /* Ignore hold (0.0.0.0): keep sending to the previous address. */
    if ((media_ip[0] != '\0') && (strcmp(media_ip, "0.0.0.0") != 0)) {
        struct sockaddr_in addr;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, media_ip, &addr.sin_addr) == 1) {
            addr.sin_port = htons((uint16_t)port);
            if (!ua->rtp_latched) {
                ua->rtp_remote = addr;
            }
        }
    }

    return 0;
}

static void reset_media(sip_ua_t *ua) {
    ua->media_active = 0;
    ua->rtp_latched = 0;
    ua->rx_started = 0;
    ua->rx_last_len = RTP_SAMPLES;
    memset(ua->rx_slots, 0, sizeof(ua->rx_slots));
    memset(&ua->rtp_remote, 0, sizeof(ua->rtp_remote));
    ua->tx_seq = (uint16_t)osal_random32();
    ua->tx_ts = osal_random32();
    ua->tx_marker = 1;
}

static void handle_invite(sip_ua_t *ua, const sip_msg_t *msg, const struct sockaddr_in *src) {
    const char *call_id = sip_header(msg, "Call-ID", 0);
    const char *from = sip_header(msg, "From", 0);
    const char *to = sip_header(msg, "To", 0);
    char to_tag[64];
    char from_uri[256];

    if ((call_id == NULL) || (from == NULL) || (to == NULL)) {
        sip_respond(ua, msg, src, 400, "Bad Request", NULL, NULL, NULL, NULL, NULL, 0);
        return;
    }

    /* Retransmission of the INVITE we are already handling. */
    if ((ua->call_state != CALL_IDLE) && (strcmp(call_id, ua->call_id) == 0)) {
        char cseq[64];

        copy_str(cseq, sizeof(cseq), sip_header(msg, "CSeq", 0));
        if (strcmp(cseq, ua->invite_cseq) == 0) {
            if (ua->last_response_len > 0) {
                sip_send(ua, src, ua->last_response, ua->last_response_len);
            }
            return;
        }

        /* re-INVITE within the dialog (session refresh, codec or address change.) */
        if (ua->call_state == CALL_CONFIRMED) {
            char sdp[1024];
            char contact[300];

            if ((msg->body_len > 0) && (parse_sdp(ua, msg->body) != 0)) {
                sip_respond(ua, msg, src, 488, "Not Acceptable Here", ua->local_tag, NULL, NULL, NULL, NULL, 0);
                return;
            }
            build_sdp(ua, sdp, sizeof(sdp));
            snprintf(contact, sizeof(contact), "Contact: <sip:%s@%s:%u>\r\n", ua->cfg.user, ua->contact_ip, ua->contact_port);
            sip_respond(ua, msg, src, 200, "OK", ua->local_tag, contact, "application/sdp", sdp, NULL, 0);
        } else {
            sip_respond(ua, msg, src, 491, "Request Pending", ua->local_tag, NULL, NULL, NULL, NULL, 0);
        }
        return;
    }

    /* Only one call at a time. */
    if (ua->call_state != CALL_IDLE) {
        char tag[17];

        random_hex(tag, sizeof(tag), 16);
        sip_respond(ua, msg, src, 486, "Busy Here", tag, NULL, NULL, NULL, NULL, 0);
        return;
    }

    /* Reject initial INVITEs that already carry a To tag (stale dialog.) */
    if (header_param(to, "tag", to_tag, sizeof(to_tag)) == 0) {
        sip_respond(ua, msg, src, 481, "Call/Transaction Does Not Exist", NULL, NULL, NULL, NULL, NULL, 0);
        return;
    }

    reset_media(ua);
    ua->payload_type = ua->cfg.codec;
    ua->sdp_offer_pending = 0;

    if (msg->body_len > 0) {
        if (parse_sdp(ua, msg->body) != 0) {
            char tag[17];

            random_hex(tag, sizeof(tag), 16);
            sip_respond(ua, msg, src, 488, "Not Acceptable Here", tag, NULL, NULL, NULL, NULL, 0);
            printf("SIP: Rejected call: no G.711 audio offered.\n");
            return;
        }
    } else {
        ua->sdp_offer_pending = 1;
    }

    /* Set up dialog state. */
    copy_str(ua->call_id, sizeof(ua->call_id), call_id);
    random_hex(ua->local_tag, sizeof(ua->local_tag), 16);
    copy_str(ua->remote_from, sizeof(ua->remote_from), from);
    copy_str(ua->local_to, sizeof(ua->local_to), to);
    copy_str(ua->invite_cseq, sizeof(ua->invite_cseq), sip_header(msg, "CSeq", 0));
    header_uri(sip_header(msg, "Contact", 0), ua->remote_target, sizeof(ua->remote_target));
    if (ua->remote_target[0] == '\0') header_uri(from, ua->remote_target, sizeof(ua->remote_target));
    ua->invite_vias[0] = '\0';
    sip_copy_headers(msg, "Via", "Via", ua->invite_vias, sizeof(ua->invite_vias));
    ua->route_set[0] = '\0';
    sip_copy_headers(msg, "Record-Route", "Route", ua->route_set, sizeof(ua->route_set));
    ua->record_route[0] = '\0';
    sip_copy_headers(msg, "Record-Route", "Record-Route", ua->record_route, sizeof(ua->record_route));
    ua->sig_addr = *src;
    ua->local_cseq = osal_random32() & 0xFFFF;
    ua->answer_requested = 0;
    ua->bye_tx.active = 0;
    ua->call_state = CALL_RINGING;

    sip_respond(ua, msg, src, 100, "Trying", NULL, NULL, NULL, NULL, NULL, 0);
    ua->last_response_len = sip_respond(ua, msg, src, 180, "Ringing", ua->local_tag, NULL, NULL, NULL,
                                        ua->last_response, sizeof(ua->last_response));

    header_uri(from, from_uri, sizeof(from_uri));
    printf("SIP: Incoming call from %s (%s).\n", from_uri, ua->payload_type == RTP_PT_PCMA ? "PCMA" : "PCMU");

    if (ua->cb.incoming_call) ua->cb.incoming_call(ua->user, from_uri);
}

/* Build a response to the pending INVITE from saved dialog state. */
static int invite_response(sip_ua_t *ua, int status, const char *reason, const char *extra, const char *content_type,
                           const char *body) {
    char msg[SIP_MAX_MSG];
    int body_len = body ? (int)strlen(body) : 0;
    int len;

    len = snprintf(msg, sizeof(msg),
                   "SIP/2.0 %d %s\r\n"
                   "%s"
                   "%s"
                   "From: %s\r\n"
                   "To: %s;tag=%s\r\n"
                   "Call-ID: %s\r\n"
                   "CSeq: %s\r\n"
                   "%s"
                   "User-Agent: " SIP_USER_AGENT "\r\n"
                   "%s%s%s"
                   "Content-Length: %d\r\n\r\n"
                   "%s",
                   status, reason,
                   ua->invite_vias,
                   (status >= 200 && status < 300) ? ua->record_route : "",
                   ua->remote_from,
                   ua->local_to, ua->local_tag,
                   ua->call_id,
                   ua->invite_cseq,
                   extra ? extra : "",
                   content_type ? "Content-Type: " : "", content_type ? content_type : "", content_type ? "\r\n" : "",
                   body_len,
                   body ? body : "");

    if (len >= (int)sizeof(msg)) len = (int)sizeof(msg) - 1;

    sip_send(ua, &ua->sig_addr, msg, len);
    if (len > (int)sizeof(ua->last_response)) len = (int)sizeof(ua->last_response);
    memcpy(ua->last_response, msg, (size_t)len);
    ua->last_response_len = len;
    return len;
}

int sip_ua_answer(sip_ua_t *ua) {
    char sdp[1024];
    char contact[400];
    uint64_t now = osal_time_ms();

    if (ua->call_state != CALL_RINGING) return -1;

    build_sdp(ua, sdp, sizeof(sdp));
    snprintf(contact, sizeof(contact),
             "Contact: <sip:%s@%s:%u>\r\nAllow: INVITE, ACK, CANCEL, BYE, OPTIONS, UPDATE\r\n",
             ua->cfg.user, ua->contact_ip, ua->contact_port);
    invite_response(ua, 200, "OK", contact, "application/sdp", sdp);

    ua->call_state = CALL_ANSWERED;
    ua->resp_interval = SIP_T1_MS;
    ua->resp_next_retx = now + ua->resp_interval;
    ua->resp_deadline = now + SIP_TIMEOUT_MS;

    /* Media starts immediately: the far end may send audio before the ACK arrives. */
    ua->media_active = !ua->sdp_offer_pending;
    printf("SIP: Call answered.\n");
    return 0;
}

static void send_bye(sip_ua_t *ua, const char *auth_hdr) {
    char msg[SIP_MAX_MSG];
    char branch[17];
    char to[600];
    int len;

    random_hex(branch, sizeof(branch), 16);
    ua->local_cseq++;
    snprintf(to, sizeof(to), "%s", ua->remote_from);

    len = snprintf(msg, sizeof(msg),
                   "BYE %s SIP/2.0\r\n"
                   "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK%s;rport\r\n"
                   "Max-Forwards: 70\r\n"
                   "%s"
                   "From: %s;tag=%s\r\n"
                   "To: %s\r\n"
                   "Call-ID: %s\r\n"
                   "CSeq: %u BYE\r\n"
                   "%s"
                   "User-Agent: " SIP_USER_AGENT "\r\n"
                   "Content-Length: 0\r\n\r\n",
                   ua->remote_target,
                   ua->contact_ip, ua->contact_port, branch,
                   ua->route_set,
                   ua->local_to, ua->local_tag,
                   to,
                   ua->call_id,
                   ua->local_cseq,
                   auth_hdr ? auth_hdr : "");

    if (len >= (int)sizeof(msg)) len = (int)sizeof(msg) - 1;

    sip_send(ua, &ua->sig_addr, msg, len);
    client_tx_start(&ua->bye_tx, "BYE", ua->call_id, ua->local_cseq, msg, len, &ua->sig_addr);
}

static void end_call(sip_ua_t *ua, const char *reason, int notify) {
    int was_active = (ua->call_state != CALL_IDLE);

    ua->call_state = CALL_IDLE;
    ua->media_active = 0;
    ua->last_response_len = 0;

    if (was_active) {
        printf("SIP: Call ended: %s.\n", reason);
        if (notify && ua->cb.call_ended) ua->cb.call_ended(ua->user, reason);
    }
}

int sip_ua_hangup(sip_ua_t *ua) {
    switch (ua->call_state) {
    case CALL_RINGING:
        invite_response(ua, 486, "Busy Here", NULL, NULL, NULL);
        end_call(ua, "rejected", 0);
        break;
    case CALL_ANSWERED:
    case CALL_CONFIRMED:
        send_bye(ua, NULL);
        end_call(ua, "local hangup", 0);
        break;
    default:
        break;
    }
    return 0;
}

static void handle_request(sip_ua_t *ua, const sip_msg_t *msg, const struct sockaddr_in *src) {
    const char *call_id = sip_header(msg, "Call-ID", 0);
    int in_dialog = (call_id != NULL) && (ua->call_state != CALL_IDLE) && (strcmp(call_id, ua->call_id) == 0);

    if (strcmp(msg->method, "INVITE") == 0) {
        handle_invite(ua, msg, src);
    } else if (strcmp(msg->method, "ACK") == 0) {
        if (in_dialog && (ua->call_state == CALL_ANSWERED)) {
            ua->call_state = CALL_CONFIRMED;
            if (ua->sdp_offer_pending) {
                if ((msg->body_len > 0) && (parse_sdp(ua, msg->body) == 0)) {
                    ua->sdp_offer_pending = 0;
                    ua->media_active = 1;
                } else {
                    send_bye(ua, NULL);
                    end_call(ua, "no SDP answer in ACK", 1);
                }
            }
        }
    } else if (strcmp(msg->method, "BYE") == 0) {
        sip_respond(ua, msg, src, in_dialog ? 200 : 481, in_dialog ? "OK" : "Call/Transaction Does Not Exist",
                    NULL, NULL, NULL, NULL, NULL, 0);
        if (in_dialog) end_call(ua, "remote hangup", 1);
    } else if (strcmp(msg->method, "CANCEL") == 0) {
        if (in_dialog && (ua->call_state == CALL_RINGING)) {
            sip_respond(ua, msg, src, 200, "OK", ua->local_tag, NULL, NULL, NULL, NULL, 0);
            invite_response(ua, 487, "Request Terminated", NULL, NULL, NULL);
            end_call(ua, "cancelled", 1);
        } else {
            sip_respond(ua, msg, src, 481, "Call/Transaction Does Not Exist", NULL, NULL, NULL, NULL, NULL, 0);
        }
    } else if (strcmp(msg->method, "OPTIONS") == 0) {
        sip_respond(ua, msg, src, 200, "OK", NULL,
                    "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, UPDATE\r\nAccept: application/sdp\r\n",
                    NULL, NULL, NULL, 0);
    } else if (strcmp(msg->method, "UPDATE") == 0) {
        if (in_dialog) {
            char sdp[1024];

            if ((msg->body_len > 0) && (parse_sdp(ua, msg->body) == 0)) {
                build_sdp(ua, sdp, sizeof(sdp));
                sip_respond(ua, msg, src, 200, "OK", NULL, NULL, "application/sdp", sdp, NULL, 0);
            } else {
                sip_respond(ua, msg, src, 200, "OK", NULL, NULL, NULL, NULL, NULL, 0);
            }
        } else {
            sip_respond(ua, msg, src, 481, "Call/Transaction Does Not Exist", NULL, NULL, NULL, NULL, NULL, 0);
        }
    } else if (strcmp(msg->method, "INFO") == 0 || strcmp(msg->method, "NOTIFY") == 0) {
        sip_respond(ua, msg, src, 200, "OK", NULL, NULL, NULL, NULL, NULL, 0);
    } else {
        sip_respond(ua, msg, src, 405, "Method Not Allowed", NULL,
                    "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, UPDATE\r\n", NULL, NULL, NULL, 0);
    }
}

static void handle_response(sip_ua_t *ua, const sip_msg_t *msg) {
    char method[32];
    int cseq = cseq_number(msg, method, sizeof(method));
    const char *call_id = sip_header(msg, "Call-ID", 0);

    if ((cseq < 0) || (call_id == NULL)) return;

    if ((strcmp(method, "REGISTER") == 0) && ua->reg_tx.active &&
        (strcmp(call_id, ua->reg_call_id) == 0) && ((uint32_t)cseq == ua->reg_tx.cseq)) {
        handle_register_response(ua, msg);
    } else if ((strcmp(method, "BYE") == 0) && ua->bye_tx.active &&
               (strcmp(call_id, ua->bye_tx.call_id) == 0) && ((uint32_t)cseq == ua->bye_tx.cseq)) {
        if (msg->status >= 200) {
            ua->bye_tx.active = 0;
            if (((msg->status == 401) || (msg->status == 407)) && (ua->bye_tx.auth_tries++ == 0)) {
                char auth[1024];

                if (build_auth_header(ua, msg, "BYE", ua->remote_target, auth, sizeof(auth)) == 0) {
                    int tries = ua->bye_tx.auth_tries;

                    send_bye(ua, auth);
                    ua->bye_tx.auth_tries = tries;
                }
            }
        }
    }
}

/* Drop SIP and RTP from anything but the PBX (anything reaching the modem can act as a terminal). */
static int peer_allowed(sip_ua_t *ua, const struct sockaddr_in *src) {
    if (!ua->allow_set || (src->sin_addr.s_addr == ua->allow_addr.s_addr)) {
        return 1;
    }
    if ((ua->dropped_packets++ % 1000) == 0) {
        fprintf(stderr, "SIP: dropped packet(s) from an address other than the PBX (%u so far).\n",
                ua->dropped_packets);
    }
    return 0;
}

static void handle_sip_packet(sip_ua_t *ua, const char *data, int len, const struct sockaddr_in *src) {
    static sip_msg_t msg;   /* Only used from the SIP thread. */

    /* Keep-alive (CRLF) packets */
    if ((len <= 4) && ((data[0] == '\r') || (data[0] == '\n') || (data[0] == '\0'))) {
        return;
    }

    if (sip_parse(&msg, data, len) != 0) {
        if (ua->cfg.debug) fprintf(stderr, "SIP: Unparseable message ignored.\n");
        return;
    }

    if (ua->cfg.debug > 1) {
        char addr[32];

        addr_to_str(src, addr, sizeof(addr));
        printf("SIP: <<< %s\n%.*s\n", addr, len, data);
    }

    if (msg.is_request) {
        handle_request(ua, &msg, src);
    } else {
        handle_response(ua, &msg);
    }
}

/*
 * RTP
 */

static void rtp_deliver(sip_ua_t *ua, const uint8_t *payload, int len) {
    int16_t amp[RTP_MAX_PAYLOAD];
    int i;

    if (len > RTP_MAX_PAYLOAD) len = RTP_MAX_PAYLOAD;

    for (i = 0; i < len; i++) {
        amp[i] = (ua->payload_type == RTP_PT_PCMA) ? sm_alaw_to_linear(payload[i]) : sm_ulaw_to_linear(payload[i]);
    }
    ua->rx_last_len = len;

    if (ua->cb.rx_audio) ua->cb.rx_audio(ua->user, amp, len);
}

static void rtp_deliver_silence(sip_ua_t *ua) {
    int16_t amp[RTP_MAX_PAYLOAD];

    memset(amp, 0, sizeof(amp));
    if (ua->cb.rx_audio) ua->cb.rx_audio(ua->user, amp, ua->rx_last_len);
}

/* Deliver in-order packets from the re-ordering buffer, skipping over lost ones. */
static void rtp_drain(sip_ua_t *ua, int flush) {
    for (;;) {
        rtp_slot_t *slot = &ua->rx_slots[ua->rx_expected % RTP_REORDER_SLOTS];
        int waiting = 0;
        int i;

        if (slot->used && (slot->seq == ua->rx_expected)) {
            rtp_deliver(ua, slot->payload, slot->len);
            slot->used = 0;
            ua->rx_expected++;
            continue;
        }

        for (i = 0; i < RTP_REORDER_SLOTS; i++) {
            if (ua->rx_slots[i].used) waiting++;
        }

        if ((waiting == 0) || (!flush && (waiting < RTP_REORDER_DEPTH))) {
            break;
        }

        /* Packet presumed lost: fill the gap with silence. */
        rtp_deliver_silence(ua);
        ua->rx_expected++;
    }
}

static void handle_rtp_packet(sip_ua_t *ua, const uint8_t *data, int len, const struct sockaddr_in *src) {
    int header_len;
    int pt;
    uint16_t seq;
    int16_t diff;
    rtp_slot_t *slot;

    if (!ua->media_active || (len < 12) || ((data[0] >> 6) != 2)) return;

    header_len = 12 + 4 * (data[0] & 0x0F);
    if (data[0] & 0x10) {
        /* Header extension */
        if (len < header_len + 4) return;
        header_len += 4 + 4 * ((data[header_len + 2] << 8) | data[header_len + 3]);
    }
    if (data[0] & 0x20) {
        /* Padding */
        len -= data[len - 1];
    }
    if (len <= header_len) return;

    pt = data[1] & 0x7F;
    if (pt != ua->payload_type) {
        /* Comfort noise, telephone-events, etc. are ignored. */
        return;
    }

    /* Symmetric RTP: send to wherever the far end's media comes from (NAT traversal.) */
    if (!ua->rtp_latched) {
        if ((src->sin_addr.s_addr != ua->rtp_remote.sin_addr.s_addr) || (src->sin_port != ua->rtp_remote.sin_port)) {
            char addr[32];

            addr_to_str(src, addr, sizeof(addr));
            if (ua->cfg.debug) printf("SIP: RTP latched to %s.\n", addr);
        }
        ua->rtp_remote = *src;
        ua->rtp_latched = 1;
    }

    seq = (uint16_t)((data[2] << 8) | data[3]);

    if (!ua->rx_started) {
        ua->rx_started = 1;
        ua->rx_expected = seq;
    }

    diff = (int16_t)(seq - ua->rx_expected);
    if (diff < 0) {
        return;     /* Late or duplicate */
    }

    if (diff >= RTP_REORDER_SLOTS) {
        /* Large jump: flush what we have and resynchronize. */
        rtp_drain(ua, 1);
        memset(ua->rx_slots, 0, sizeof(ua->rx_slots));
        ua->rx_expected = seq;
    }

    slot = &ua->rx_slots[seq % RTP_REORDER_SLOTS];
    slot->used = 1;
    slot->seq = seq;
    slot->len = len - header_len;
    if (slot->len > RTP_MAX_PAYLOAD) slot->len = RTP_MAX_PAYLOAD;
    memcpy(slot->payload, data + header_len, (size_t)slot->len);

    rtp_drain(ua, 0);
}

int sip_ua_send_audio(sip_ua_t *ua, const int16_t *amp, int len) {
    uint8_t pkt[12 + RTP_MAX_PAYLOAD];
    int i;

    if (!ua->media_active || (ua->rtp_remote.sin_port == 0)) return -1;
    if (len > RTP_MAX_PAYLOAD) len = RTP_MAX_PAYLOAD;

    pkt[0] = 0x80;
    pkt[1] = (uint8_t)(ua->payload_type | (ua->tx_marker ? 0x80 : 0));
    pkt[2] = (uint8_t)(ua->tx_seq >> 8);
    pkt[3] = (uint8_t)ua->tx_seq;
    pkt[4] = (uint8_t)(ua->tx_ts >> 24);
    pkt[5] = (uint8_t)(ua->tx_ts >> 16);
    pkt[6] = (uint8_t)(ua->tx_ts >> 8);
    pkt[7] = (uint8_t)ua->tx_ts;
    pkt[8] = (uint8_t)(ua->tx_ssrc >> 24);
    pkt[9] = (uint8_t)(ua->tx_ssrc >> 16);
    pkt[10] = (uint8_t)(ua->tx_ssrc >> 8);
    pkt[11] = (uint8_t)ua->tx_ssrc;

    for (i = 0; i < len; i++) {
        pkt[12 + i] = (ua->payload_type == RTP_PT_PCMA) ? sm_linear_to_alaw(amp[i]) : sm_linear_to_ulaw(amp[i]);
    }

    udp_send(ua, ua->rtp_sock, &ua->rtp_remote, pkt, 12 + len);

    ua->tx_marker = 0;
    ua->tx_seq++;
    ua->tx_ts += (uint32_t)len;
    return 0;
}

/*
 * User agent
 */

sip_ua_t *sip_ua_create(const sip_config_t *cfg, const sip_callbacks_t *cb, void *user) {
    sip_ua_t *ua;
    int i;

    if (osal_net_init() != 0) return NULL;

    ua = (sip_ua_t *)calloc(1, sizeof(sip_ua_t));
    if (ua == NULL) return NULL;

    ua->cfg = *cfg;
    if (cb != NULL) ua->cb = *cb;
    ua->user = user;
    ua->tx_ssrc = osal_random32();
    ua->payload_type = cfg->codec;
    ua->rtp_sock = OSAL_INVALID_SOCKET;

    if (inet_pton(AF_INET, cfg->bind_ip[0] ? cfg->bind_ip : "127.0.0.1", &ua->bind_addr) != 1) {
        free(ua);
        return NULL;
    }

    ua->sip_sock = open_udp(ua->bind_addr, cfg->local_port);
    if (ua->sip_sock == OSAL_INVALID_SOCKET) {
        fprintf(stderr, "SIP: Unable to bind UDP port %u.\n", cfg->local_port);
        free(ua);
        return NULL;
    }

    /* Find an RTP port (even numbers only.) */
    for (i = 0; i < 50; i++) {
        uint16_t port = (uint16_t)((cfg->rtp_port & ~1u) + 2 * i);

        ua->rtp_sock = open_udp(ua->bind_addr, port);
        if (ua->rtp_sock != OSAL_INVALID_SOCKET) {
            ua->rtp_port = port;
            break;
        }
    }
    if (ua->rtp_sock == OSAL_INVALID_SOCKET) {
        fprintf(stderr, "SIP: Unable to bind an RTP port near %u.\n", cfg->rtp_port);
        osal_close_socket(ua->sip_sock);
        free(ua);
        return NULL;
    }

    ua->contact_port = cfg->local_port;

    if (cfg->registrar[0] != '\0') {
        if (resolve_ipv4(cfg->registrar, cfg->registrar_port, &ua->registrar_addr) != 0) {
            fprintf(stderr, "SIP: Unable to resolve registrar '%s'.\n", cfg->registrar);
            sip_ua_destroy(ua);
            return NULL;
        }
    }

    /* Only the PBX may send SIP or RTP: an explicit allow, else the registrar. */
    if (cfg->allow_ip[0] != '\0') {
        ua->allow_set = (inet_pton(AF_INET, cfg->allow_ip, &ua->allow_addr) == 1);
    } else if (cfg->registrar[0] != '\0') {
        ua->allow_addr = ua->registrar_addr.sin_addr;
        ua->allow_set = 1;
    }

    if (cfg->local_ip[0] != '\0') {
        copy_str(ua->contact_ip, sizeof(ua->contact_ip), cfg->local_ip);
    } else {
        struct sockaddr_in probe;

        if (cfg->registrar[0] != '\0') {
            probe = ua->registrar_addr;
        } else {
            memset(&probe, 0, sizeof(probe));
            probe.sin_family = AF_INET;
            probe.sin_port = htons(53);
            inet_pton(AF_INET, "192.0.2.1", &probe.sin_addr);  /* Any routable address will do */
        }
        if (local_ip_for(&probe, ua->contact_ip, sizeof(ua->contact_ip)) != 0) {
            copy_str(ua->contact_ip, sizeof(ua->contact_ip), "127.0.0.1");
        }
    }

    printf("SIP: Listening on %s:%u (RTP port %u), user '%s'.\n",
           ua->contact_ip, ua->contact_port, ua->rtp_port, cfg->user);

    if (cfg->registrar[0] != '\0') {
        random_hex(ua->reg_call_id, sizeof(ua->reg_call_id), 24);
        random_hex(ua->reg_from_tag, sizeof(ua->reg_from_tag), 16);
        ua->reg_cseq = 0;
        ua->next_register = 0;     /* Register immediately */
    } else {
        printf("SIP: No registrar configured; accepting direct calls to sip:%s@%s:%u.\n",
               cfg->user, ua->contact_ip, ua->contact_port);
    }

    return ua;
}

void sip_ua_destroy(sip_ua_t *ua) {
    if (ua == NULL) return;

    if (ua->call_state != CALL_IDLE) {
        sip_ua_hangup(ua);
    }

    /* Un-register. */
    if (ua->registered && (ua->cfg.registrar[0] != '\0')) {
        ua->cfg.expires = 0;
        sip_send_register(ua, NULL);
    }

    if (ua->sip_sock != OSAL_INVALID_SOCKET) osal_close_socket(ua->sip_sock);
    if (ua->rtp_sock != OSAL_INVALID_SOCKET) osal_close_socket(ua->rtp_sock);
    free(ua);
}

int sip_ua_media_active(const sip_ua_t *ua) {
    return ua->media_active;
}

int sip_ua_registered(const sip_ua_t *ua) {
    return (ua->cfg.registrar[0] == '\0') || ua->registered;
}

void sip_ua_poll(sip_ua_t *ua, uint32_t timeout_ms) {
    fd_set rfds;
    struct timeval tv;
    osal_socket_t maxfd;
    uint64_t now;
    int n;

    FD_ZERO(&rfds);
    FD_SET(ua->sip_sock, &rfds);
    FD_SET(ua->rtp_sock, &rfds);
    maxfd = (ua->sip_sock > ua->rtp_sock) ? ua->sip_sock : ua->rtp_sock;

    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;

    n = select((int)maxfd + 1, &rfds, NULL, NULL, &tv);

    if (n > 0) {
        char buf[SIP_MAX_MSG];
        struct sockaddr_in src;
        osal_socklen_t srclen;
        int len;

        if (FD_ISSET(ua->rtp_sock, &rfds)) {
            /* Drain all queued RTP packets. */
            for (int i = 0; i < 20; i++) {
                fd_set one;
                struct timeval zero = { 0, 0 };

                srclen = (osal_socklen_t)sizeof(src);
                len = (int)recvfrom(ua->rtp_sock, buf, (int)sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
                if ((len > 0) && peer_allowed(ua, &src)) handle_rtp_packet(ua, (const uint8_t *)buf, len, &src);

                FD_ZERO(&one);
                FD_SET(ua->rtp_sock, &one);
                if (select((int)ua->rtp_sock + 1, &one, NULL, NULL, &zero) <= 0) break;
            }
        }

        if (FD_ISSET(ua->sip_sock, &rfds)) {
            srclen = (osal_socklen_t)sizeof(src);
            len = (int)recvfrom(ua->sip_sock, buf, (int)sizeof(buf) - 1, 0, (struct sockaddr *)&src, &srclen);
            if ((len > 0) && peer_allowed(ua, &src)) handle_sip_packet(ua, buf, len, &src);
        }
    }

    /* Timers */
    now = osal_time_ms();

    if ((ua->cfg.registrar[0] != '\0') && !ua->reg_tx.active && (now >= ua->next_register)) {
        ua->reg_tx.auth_tries = 0;
        sip_send_register(ua, NULL);
    }

    if (client_tx_timer(ua, &ua->reg_tx, now)) {
        if (ua->registered || (ua->next_register == 0)) {
            fprintf(stderr, "SIP: Registrar %s not responding, retrying in 30 seconds.\n", ua->cfg.registrar);
        }
        ua->registered = 0;
        ua->next_register = now + 30000;
    }

    client_tx_timer(ua, &ua->bye_tx, now);

    /* Retransmit 200 OK until ACK is received. */
    if ((ua->call_state == CALL_ANSWERED) && (now >= ua->resp_next_retx)) {
        if (now >= ua->resp_deadline) {
            send_bye(ua, NULL);
            end_call(ua, "no ACK received", 1);
        } else {
            sip_send(ua, &ua->sig_addr, ua->last_response, ua->last_response_len);
            ua->resp_interval *= 2;
            if (ua->resp_interval > SIP_T2_MS) ua->resp_interval = SIP_T2_MS;
            ua->resp_next_retx = now + ua->resp_interval;
        }
    }
}

/*
 * Tests for the SIP virtual modem's network exposure controls (fork-only).
 *
 * The SIP/RTP line carries card numbers and PINs, and anything that can
 * reach the modem can act as a terminal, so:
 *   - the sockets never bind the wildcard address (bind defaults to 127.0.0.1);
 *   - "listen" on a non-loopback address requires allow=<PBX address>;
 *   - SIP and RTP from any address other than allow (or the registrar) are dropped.
 *
 * See docs/softmodem/ADR-0003-SIP-SOFTMODEM.md.
 */

#include <stdio.h>
#include <string.h>

#include "../src/mm_osal.h"
#include "../src/mm_sip.h"

static int failures;

#define CHECK(cond, what) do { \
        if (!(cond)) { fprintf(stderr, "FAIL: %s\n", what); failures++; } \
        else { printf("ok: %s\n", what); } \
    } while (0)

static void test_parse(void) {
    sip_config_t cfg;

    CHECK(sip_config_parse(&cfg, "listen") == 0, "listen parses");
    CHECK(strcmp(cfg.bind_ip, "127.0.0.1") == 0, "bind defaults to 127.0.0.1");
    CHECK(strcmp(cfg.local_ip, "127.0.0.1") == 0, "local_ip defaults to bind address");

    CHECK(sip_config_parse(&cfg, "listen;bind=0.0.0.0") != 0, "bind=0.0.0.0 refused");
    CHECK(sip_config_parse(&cfg, "listen;bind=not-an-ip") != 0, "bind=<hostname> refused");
    CHECK(sip_config_parse(&cfg, "listen;bind=192.0.2.5") != 0,
          "listen on non-loopback without allow refused");
    CHECK(sip_config_parse(&cfg, "listen;bind=192.0.2.5;allow=192.0.2.1") == 0,
          "listen on non-loopback with allow accepted");
    CHECK(strcmp(cfg.local_ip, "192.0.2.5") == 0, "local_ip follows bind");
    CHECK(sip_config_parse(&cfg, "listen;allow=pbx.example") != 0, "allow=<hostname> refused");
    CHECK(sip_config_parse(&cfg, "1000@192.0.2.1;bind=192.0.2.5") == 0,
          "registrar mode on non-loopback accepted (registrar is the allowed peer)");
}

static osal_socket_t udp_from(const char *ip) {
    osal_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;

    if (s == OSAL_INVALID_SOCKET) return s;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (bind(s, (const struct sockaddr *)&a, (osal_socklen_t)sizeof(a)) != 0) {
        osal_close_socket(s);
        return OSAL_INVALID_SOCKET;
    }
    return s;
}

/* Send OPTIONS from src_ip to the UA and report whether it answered. */
static int options_answered(sip_ua_t *ua, const char *src_ip, uint16_t ua_port) {
    osal_socket_t s = udp_from(src_ip);
    struct sockaddr_in dst;
    char msg[512];
    char buf[2048];
    int len;
    int answered = 0;

    if (s == OSAL_INVALID_SOCKET) return -1;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(ua_port);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    len = snprintf(msg, sizeof(msg),
                   "OPTIONS sip:mm@127.0.0.1:%u SIP/2.0\r\n"
                   "Via: SIP/2.0/UDP %s:5999;branch=z9hG4bKopt%s\r\n"
                   "From: <sip:probe@%s>;tag=p1\r\n"
                   "To: <sip:mm@127.0.0.1>\r\n"
                   "Call-ID: opt-%s\r\nCSeq: 1 OPTIONS\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n",
                   ua_port, src_ip, src_ip, src_ip, src_ip);
    sendto(s, msg, len, 0, (const struct sockaddr *)&dst, (osal_socklen_t)sizeof(dst));

    for (int i = 0; i < 20 && !answered; i++) {
        fd_set rfds;
        struct timeval tv = { 0, 0 };

        sip_ua_poll(ua, 50);
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        if (select((int)s + 1, &rfds, NULL, NULL, &tv) > 0) {
            if (recv(s, buf, sizeof(buf) - 1, 0) > 0 && strncmp(buf, "SIP/2.0 ", 8) == 0) {
                answered = 1;
            }
        }
    }
    osal_close_socket(s);
    return answered;
}

static void test_peer_filter(void) {
    sip_config_t cfg;
    sip_ua_t *ua;
    int r;

    /* The PBX is 127.0.0.2; a local process on 127.0.0.1 must be ignored. */
    if (sip_config_parse(&cfg, "listen;port=25162;rtp_port=26100;allow=127.0.0.2") != 0) {
        CHECK(0, "parse for peer filter test");
        return;
    }
    ua = sip_ua_create(&cfg, NULL, NULL);
    CHECK(ua != NULL, "UA created on 127.0.0.1");
    if (ua == NULL) return;

    CHECK(options_answered(ua, "127.0.0.1", 25162) == 0, "OPTIONS from a non-allowed address is dropped");

    r = options_answered(ua, "127.0.0.2", 25162);
    if (r < 0) {
        printf("skip: cannot bind 127.0.0.2 on this host (only 127.0.0.1 configured)\n");
    } else {
        CHECK(r == 1, "OPTIONS from the allowed address is answered");
    }
    sip_ua_destroy(ua);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (osal_net_init() != 0) return 1;

    test_parse();
    test_peer_filter();

    printf("%s\n", failures ? "SIP access test FAILED" : "SIP access test PASSED");
    return failures ? 1 : 0;
}

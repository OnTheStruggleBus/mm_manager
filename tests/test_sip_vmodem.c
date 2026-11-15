/*
 * End-to-end test for the SIP virtual modem.
 *
 * The test plays the part of a SIP registrar / PBX, the ATA and the
 * Millennium terminal's Bell 212A modem (using the originate side of the
 * software modem), and talks over real UDP sockets on the loopback
 * interface to a virtual modem, driven through the same serial-style
 * interface mm_manager uses:
 *
 *   1. The virtual modem registers, and is challenged for digest auth.
 *   2. The "terminal" calls it: INVITE / 100 / 180 / 200 / ACK.
 *   3. The host sees RING and CONNECT, and data is exchanged both ways
 *      over RTP (with some packets re-ordered.)
 *   4. The host drops DTR, and the virtual modem sends BYE.
 *
 * Part of mmumblephone, a fork of mm_manager by Howard M. Harte
 * (www.github.com/hharte/mm_manager).
 *
 * Copyright (c) 2026, mmumblephone contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "src/mm_osal.h"
#include "src/mm_sip.h"
#include "src/mm_softmodem.h"
#include "src/mm_vmodem.h"

#define REGISTRAR_PORT  (25060)
#define VMODEM_PORT     (25062)
#define VMODEM_RTP_PORT (26000)
#define CALLER_RTP_PORT (26100)
#define TEST_BYTES      (120)
#define TEST_TIMEOUT_MS (40000)

static uint8_t  term_tx[TEST_BYTES];    /* Terminal -> host */
static uint8_t  host_tx[TEST_BYTES];    /* Host -> terminal */
static uint8_t  term_rx[TEST_BYTES * 2];
static int      term_rx_len;
static int      term_tx_pos;
static int      term_send_enabled;
static volatile int host_rx_done;

static volatile int result_auth_ok;
static volatile int result_registered;
static volatile int result_answered;
static volatile int result_bye;
static volatile int network_failed;

static osal_socket_t open_socket(uint16_t port) {
    osal_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(s, (struct sockaddr *)&addr, (osal_socklen_t)sizeof(addr)) != 0) {
        fprintf(stderr, "bind(%u) failed\n", port);
        exit(1);
    }
    return s;
}

static int recv_timeout(osal_socket_t s, char *buf, int size, uint32_t timeout_ms, struct sockaddr_in *src) {
    fd_set rfds;
    struct timeval tv;
    osal_socklen_t len = (osal_socklen_t)sizeof(*src);
    int n;

    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;
    if (select((int)s + 1, &rfds, NULL, NULL, &tv) <= 0) return 0;

    n = (int)recvfrom(s, buf, size - 1, 0, (struct sockaddr *)src, &len);
    if (n > 0) buf[n] = '\0';
    return n;
}

static void send_to(osal_socket_t s, const struct sockaddr_in *dst, const char *buf, int len) {
    sendto(s, buf, len, 0, (const struct sockaddr *)dst, (osal_socklen_t)sizeof(*dst));
}

/* Copy the value of a header (first occurrence) into out. */
static void get_header(const char *msg, const char *name, char *out, size_t size) {
    char pattern[64];
    const char *p;
    size_t n;

    out[0] = '\0';
    snprintf(pattern, sizeof(pattern), "\r\n%s:", name);
    p = strstr(msg, pattern);
    if (p == NULL) return;
    p += strlen(pattern);
    while (*p == ' ') p++;
    n = strcspn(p, "\r\n");
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void get_param(const char *str, const char *name, char *out, size_t size) {
    char pattern[64];
    const char *p;
    size_t n;

    out[0] = '\0';
    snprintf(pattern, sizeof(pattern), "%s=\"", name);
    p = strstr(str, pattern);
    if (p == NULL) return;
    p += strlen(pattern);
    n = strcspn(p, "\"");
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Build a response to a request from the virtual modem. */
static int respond(const char *req, int status, const char *reason, const char *extra, char *out, size_t size) {
    char via[256], from[256], to[256], call_id[128], cseq[64];

    get_header(req, "Via", via, sizeof(via));
    get_header(req, "From", from, sizeof(from));
    get_header(req, "To", to, sizeof(to));
    get_header(req, "Call-ID", call_id, sizeof(call_id));
    get_header(req, "CSeq", cseq, sizeof(cseq));

    return snprintf(out, size,
                    "SIP/2.0 %d %s\r\nVia: %s;received=127.0.0.1\r\nFrom: %s\r\nTo: %s;tag=reg1\r\n"
                    "Call-ID: %s\r\nCSeq: %s\r\n%sContent-Length: 0\r\n\r\n",
                    status, reason, via, from, to, call_id, cseq, extra ? extra : "");
}

/* Terminal modem callbacks */
static int term_get_byte(void *user) {
    (void)user;
    if (term_send_enabled && (term_tx_pos < TEST_BYTES)) return term_tx[term_tx_pos++];
    return -1;
}

static void term_put_byte(void *user, uint8_t byte) {
    (void)user;
    if (term_rx_len < (int)sizeof(term_rx)) term_rx[term_rx_len++] = byte;
}

static int check_register(osal_socket_t s) {
    char buf[4096], resp[4096], auth[1024];
    struct sockaddr_in src;
    int n;

    /* First REGISTER: challenge it. */
    n = recv_timeout(s, buf, (int)sizeof(buf), 5000, &src);
    if ((n <= 0) || (strncmp(buf, "REGISTER sip:127.0.0.1 SIP/2.0", 30) != 0)) {
        fprintf(stderr, "Expected REGISTER, got: %.*s\n", n > 0 ? n : 0, buf);
        return -1;
    }
    n = respond(buf, 401, "Unauthorized",
                "WWW-Authenticate: Digest realm=\"test\", nonce=\"abc123\", qop=\"auth\", algorithm=MD5\r\n",
                resp, sizeof(resp));
    send_to(s, &src, resp, n);

    /* Second REGISTER must carry a correct digest response. */
    n = recv_timeout(s, buf, (int)sizeof(buf), 5000, &src);
    if (n <= 0) return -1;
    get_header(buf, "Authorization", auth, sizeof(auth));
    {
        char username[64], realm[64], nonce[64], uri[128], response[64], cnonce[64];
        char ha1_in[256], ha1[33], ha2_in[256], ha2[33], resp_in[512], expected[33];

        get_param(auth, "username", username, sizeof(username));
        get_param(auth, "realm", realm, sizeof(realm));
        get_param(auth, "nonce", nonce, sizeof(nonce));
        get_param(auth, "uri", uri, sizeof(uri));
        get_param(auth, "response", response, sizeof(response));
        get_param(auth, "cnonce", cnonce, sizeof(cnonce));

        snprintf(ha1_in, sizeof(ha1_in), "%s:%s:%s", "1000", "test", "secret");
        sip_md5_hex(ha1_in, ha1);
        snprintf(ha2_in, sizeof(ha2_in), "REGISTER:%s", uri);
        sip_md5_hex(ha2_in, ha2);
        snprintf(resp_in, sizeof(resp_in), "%s:%s:00000001:%s:auth:%s", ha1, "abc123", cnonce, ha2);
        sip_md5_hex(resp_in, expected);

        result_auth_ok = (strcmp(username, "1000") == 0) && (strcmp(nonce, "abc123") == 0) &&
                         (strcmp(response, expected) == 0) && (strstr(auth, "nc=00000001") != NULL);
        printf("Registrar: digest response %s\n", result_auth_ok ? "verified" : "WRONG");
    }

    n = respond(buf, 200, "OK", "Contact: <sip:1000@127.0.0.1:25062>;expires=300\r\n", resp, sizeof(resp));
    send_to(s, &src, resp, n);
    result_registered = 1;
    return 0;
}

static osal_thread_ret_t OSAL_THREAD_CALL network_thread(void *arg) {
    osal_socket_t sip = open_socket(REGISTRAR_PORT);
    osal_socket_t rtp = open_socket(CALLER_RTP_PORT);
    struct sockaddr_in vm_sip, vm_rtp, src;
    char buf[4096], msg[4096], to[256], sdp[512];
    softmodem_t *term;
    uint64_t start = osal_time_ms();
    uint64_t next_tick;
    uint64_t data_time = 0;
    uint8_t held_pkt[200];
    int held_len = 0;
    int pkt_count = 0;
    int n;

    (void)arg;

    if (check_register(sip) != 0) {
        network_failed = 1;
        return (osal_thread_ret_t)0;
    }

    memset(&vm_sip, 0, sizeof(vm_sip));
    vm_sip.sin_family = AF_INET;
    vm_sip.sin_port = htons(VMODEM_PORT);
    inet_pton(AF_INET, "127.0.0.1", &vm_sip.sin_addr);

    /* Place the call. */
    n = snprintf(sdp, sizeof(sdp),
                 "v=0\r\no=ata 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                 "m=audio %d RTP/AVP 0 8 101\r\na=rtpmap:101 telephone-event/8000\r\n", CALLER_RTP_PORT);
    n = snprintf(msg, sizeof(msg),
                 "INVITE sip:1000@127.0.0.1:%d SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bKinv1\r\n"
                 "Max-Forwards: 70\r\n"
                 "From: \"Payphone\" <sip:5551212@127.0.0.1>;tag=caller1\r\n"
                 "To: <sip:1000@127.0.0.1>\r\n"
                 "Call-ID: call-1@127.0.0.1\r\n"
                 "CSeq: 1 INVITE\r\n"
                 "Contact: <sip:5551212@127.0.0.1:%d>\r\n"
                 "Content-Type: application/sdp\r\n"
                 "Content-Length: %d\r\n\r\n%s",
                 VMODEM_PORT, REGISTRAR_PORT, REGISTRAR_PORT, n, sdp);
    send_to(sip, &vm_sip, msg, n);

    /* Wait for 200 OK. */
    to[0] = '\0';
    while (osal_time_ms() - start < 15000) {
        n = recv_timeout(sip, buf, (int)sizeof(buf), 1000, &src);
        if (n <= 0) continue;
        if (strncmp(buf, "SIP/2.0 200", 11) == 0 && strstr(buf, "INVITE")) {
            const char *m = strstr(buf, "m=audio ");

            get_header(buf, "To", to, sizeof(to));
            memset(&vm_rtp, 0, sizeof(vm_rtp));
            vm_rtp.sin_family = AF_INET;
            vm_rtp.sin_port = htons((uint16_t)(m ? atoi(m + 8) : 0));
            inet_pton(AF_INET, "127.0.0.1", &vm_rtp.sin_addr);
            result_answered = (m != NULL) && (strstr(buf, "a=gpmd:0 vbd=yes") != NULL);
            printf("Caller: call answered, RTP port %d\n", ntohs(vm_rtp.sin_port));
            break;
        } else if (strncmp(buf, "SIP/2.0 1", 9) == 0) {
            printf("Caller: %.*s\n", (int)strcspn(buf, "\r\n"), buf);
        }
    }
    if (to[0] == '\0') {
        fprintf(stderr, "Caller: no 200 OK.\n");
        network_failed = 1;
        return (osal_thread_ret_t)0;
    }

    n = snprintf(msg, sizeof(msg),
                 "ACK sip:1000@127.0.0.1:%d SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bKack1\r\n"
                 "Max-Forwards: 70\r\n"
                 "From: \"Payphone\" <sip:5551212@127.0.0.1>;tag=caller1\r\n"
                 "To: %s\r\n"
                 "Call-ID: call-1@127.0.0.1\r\n"
                 "CSeq: 1 ACK\r\n"
                 "Content-Length: 0\r\n\r\n",
                 VMODEM_PORT, REGISTRAR_PORT, to);
    send_to(sip, &vm_sip, msg, n);

    /* Run the terminal's modem over RTP. */
    term = softmodem_create(0, SM_ANSWER_TONE_BELL, term_get_byte, term_put_byte, NULL);
    next_tick = osal_time_ms();

    while ((osal_time_ms() - start < TEST_TIMEOUT_MS) && !result_bye) {
        uint64_t now = osal_time_ms();

        if (now >= next_tick) {
            uint8_t pkt[12 + 160];
            int16_t amp[160];
            static uint16_t seq = 1000;
            static uint32_t ts = 0;

            softmodem_tx(term, amp, 160);
            pkt[0] = 0x80; pkt[1] = 0; pkt[2] = (uint8_t)(seq >> 8); pkt[3] = (uint8_t)seq;
            pkt[4] = (uint8_t)(ts >> 24); pkt[5] = (uint8_t)(ts >> 16); pkt[6] = (uint8_t)(ts >> 8); pkt[7] = (uint8_t)ts;
            pkt[8] = 0x11; pkt[9] = 0x22; pkt[10] = 0x33; pkt[11] = 0x44;
            for (int i = 0; i < 160; i++) pkt[12 + i] = sm_linear_to_ulaw(amp[i]);
            seq++;
            ts += 160;

            /* Swap every 25th packet with the following one to exercise re-ordering. */
            if (held_len > 0) {
                send_to(rtp, &vm_rtp, (const char *)pkt, (int)sizeof(pkt));
                send_to(rtp, &vm_rtp, (const char *)held_pkt, held_len);
                held_len = 0;
            } else if ((++pkt_count % 25) == 0) {
                memcpy(held_pkt, pkt, sizeof(pkt));
                held_len = (int)sizeof(pkt);
            } else {
                send_to(rtp, &vm_rtp, (const char *)pkt, (int)sizeof(pkt));
            }
            next_tick += 20;

            if ((softmodem_state(term) == SM_STATE_DATA) && (data_time == 0)) {
                data_time = now;
                printf("Terminal: modem connected after %u ms.\n", (unsigned)(now - start));
            }
            /* Like the Millennium, start sending once connected. */
            if ((data_time != 0) && (now - data_time > 300)) term_send_enabled = 1;
        }

        n = recv_timeout(rtp, buf, (int)sizeof(buf), 1, &src);
        if (n > 12) {
            int16_t amp[400];
            int len = n - 12;

            if (len > 400) len = 400;
            for (int i = 0; i < len; i++) amp[i] = sm_ulaw_to_linear((uint8_t)buf[12 + i]);
            softmodem_rx(term, amp, len);
        }

        n = recv_timeout(sip, buf, (int)sizeof(buf), 0, &src);
        if ((n > 0) && (strncmp(buf, "BYE ", 4) == 0)) {
            char resp[4096];
            int len = snprintf(resp, sizeof(resp), "SIP/2.0 200 OK\r\n");
            char via[256], from[256], tohdr[256], call_id[128], cseq[64];

            get_header(buf, "Via", via, sizeof(via));
            get_header(buf, "From", from, sizeof(from));
            get_header(buf, "To", tohdr, sizeof(tohdr));
            get_header(buf, "Call-ID", call_id, sizeof(call_id));
            get_header(buf, "CSeq", cseq, sizeof(cseq));
            len += snprintf(resp + len, sizeof(resp) - (size_t)len,
                            "Via: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\nContent-Length: 0\r\n\r\n",
                            via, from, tohdr, call_id, cseq);
            send_to(sip, &src, resp, len);
            result_bye = (strcmp(call_id, "call-1@127.0.0.1") == 0) && (strstr(tohdr, "caller1") != NULL);
            printf("Caller: received BYE.\n");
        } else if ((n > 0) && (strncmp(buf, "REGISTER ", 9) == 0)) {
            char resp[4096];
            int len = respond(buf, 200, "OK", NULL, resp, sizeof(resp));

            send_to(sip, &src, resp, len);
        }
    }

    softmodem_free(term);
    osal_close_socket(sip);
    osal_close_socket(rtp);
    return (osal_thread_ret_t)0;
}

/* Read a result code line from the virtual modem. */
static int read_line(mm_vmodem_t *vm, char *line, size_t size, uint32_t timeout_ms) {
    uint64_t deadline = osal_time_ms() + timeout_ms;
    size_t len = 0;

    while (osal_time_ms() < deadline) {
        char c;

        if (vmodem_read(vm, &c, 1) == 1) {
            if ((c == '\r') || (c == '\n')) {
                if (len > 0) {
                    line[len] = '\0';
                    return 0;
                }
            } else if (len < size - 1) {
                line[len++] = c;
            }
        }
    }
    line[len] = '\0';
    return -1;
}

static int expect(mm_vmodem_t *vm, const char *cmd, const char *response, uint32_t timeout_ms) {
    char line[256];

    if (cmd != NULL) {
        vmodem_write(vm, cmd, strlen(cmd));
    }

    while (read_line(vm, line, sizeof(line), timeout_ms) == 0) {
        printf("Host: %s%s<- %s\n", cmd ? cmd : "", cmd ? " " : "", line);
        if (strncmp(line, response, strlen(response)) == 0) return 0;
    }
    fprintf(stderr, "Host: timed out waiting for %s\n", response);
    return -1;
}

int main(void) {
    sip_config_t cfg;
    mm_vmodem_t *vm;
    osal_thread_t thread;
    char md5[33];
    int failures = 0;
    int host_rx_len = 0;
    uint8_t host_rx[TEST_BYTES];
    uint64_t start;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* MD5 test vectors (RFC 1321) */
    sip_md5_hex("", md5);
    if (strcmp(md5, "d41d8cd98f00b204e9800998ecf8427e") != 0) failures++;
    sip_md5_hex("The quick brown fox jumps over the lazy dog", md5);
    if (strcmp(md5, "9e107d9d372bb6826bd81d3542a419d6") != 0) failures++;
    sip_md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890", md5);
    if (strcmp(md5, "57edf4a22be3c955ac49da2e2107b67a") != 0) failures++;
    printf("MD5 test vectors: %s\n", failures ? "FAIL" : "PASS");

    for (int i = 0; i < TEST_BYTES; i++) {
        term_tx[i] = (uint8_t)(i * 7 + 3);
        host_tx[i] = (uint8_t)(255 - i * 3);
    }

    if (sip_config_parse(&cfg, "1000:secret@127.0.0.1:25060;port=25062;rtp_port=26000;local_ip=127.0.0.1") != 0) {
        fprintf(stderr, "sip_config_parse failed\n");
        return 1;
    }

    if (osal_net_init() != 0) return 1;
    if (osal_thread_create(&thread, network_thread, NULL) != 0) return 1;
    osal_sleep_ms(100);

    vm = vmodem_open(&cfg);
    if (vm == NULL) {
        fprintf(stderr, "vmodem_open failed\n");
        return 1;
    }

    /* Same sequence as mm_manager: reset, init, wait for RING and CONNECT. */
    if (expect(vm, "ATZ\r", "OK", 2000) != 0) failures++;
    if (expect(vm, "ATE=1 S0=1 S7=3 &D2 +MS=B212\r", "OK", 2000) != 0) failures++;
    if (expect(vm, NULL, "RING", 10000) != 0) failures++;
    if (expect(vm, NULL, "CONNECT", 15000) != 0) failures++;

    if (!(vmodem_get_modem_status(vm) & 0x80)) {
        fprintf(stderr, "Host: DCD not set after CONNECT\n");
        failures++;
    }

    /* Data mode: exchange data in both directions.  Skip the LF that follows CONNECT. */
    {
        uint8_t c;

        if ((vmodem_read(vm, &c, 1) == 1) && (c != '\n') && (c != '\r')) {
            host_rx[host_rx_len++] = c;
        }
    }
    vmodem_write(vm, host_tx, TEST_BYTES);
    vmodem_drain(vm);

    start = osal_time_ms();
    while ((host_rx_len < TEST_BYTES) && (osal_time_ms() - start < 10000)) {
        long n = vmodem_read(vm, &host_rx[host_rx_len], (size_t)(TEST_BYTES - host_rx_len));
        if (n > 0) host_rx_len += (int)n;
    }
    host_rx_done = 1;

    /* Wait for the terminal to have received everything. */
    start = osal_time_ms();
    while ((term_rx_len < TEST_BYTES) && (osal_time_ms() - start < 5000)) osal_sleep_ms(50);

    if ((host_rx_len != TEST_BYTES) || (memcmp(host_rx, term_tx, TEST_BYTES) != 0)) {
        fprintf(stderr, "Host: received %d bytes from terminal, data %s\n", host_rx_len,
                memcmp(host_rx, term_tx, TEST_BYTES) ? "MISMATCH" : "ok");
        failures++;
    }
    if ((term_rx_len != TEST_BYTES) || (memcmp(term_rx, host_tx, TEST_BYTES) != 0)) {
        fprintf(stderr, "Terminal: received %d bytes from host, data %s\n", term_rx_len,
                memcmp(term_rx, host_tx, TEST_BYTES) ? "MISMATCH" : "ok");
        failures++;
    }
    printf("Data: host received %d/%d, terminal received %d/%d\n", host_rx_len, TEST_BYTES, term_rx_len, TEST_BYTES);

    /* Hang up like mm_manager does: drop DTR. */
    vmodem_set_dtr(vm, 0);
    osal_sleep_ms(100);
    vmodem_set_dtr(vm, 1);
    if (vmodem_get_modem_status(vm) & 0x80) {
        fprintf(stderr, "Host: DCD still set after hangup\n");
        failures++;
    }

    osal_thread_join(thread);
    vmodem_close(vm);

    if (network_failed) failures++;
    if (!result_auth_ok) { fprintf(stderr, "Digest authentication failed\n"); failures++; }
    if (!result_registered) { fprintf(stderr, "Registration failed\n"); failures++; }
    if (!result_answered) { fprintf(stderr, "Call not answered with expected SDP\n"); failures++; }
    if (!result_bye) { fprintf(stderr, "BYE not received\n"); failures++; }

    printf("%s\n", failures ? "SIP virtual modem test FAILED" : "SIP virtual modem test PASSED");
    return failures ? 1 : 0;
}

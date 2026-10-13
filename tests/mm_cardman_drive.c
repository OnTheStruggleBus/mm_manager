/*
 * Drives the CARDMAN glue (src/mm_cardman.c) with synthetic terminal
 * records, against whatever bridge CARDMAN_BRIDGE_ADDR points at. Builds
 * the same packed wire structs process_mm_table() hands to the glue, so
 * everything after DLOG framing is the production code path.
 *
 *   mm_cardman_drive auth <terminal> <pan> <pin> <dialed> <seq> <call_type> [repeat]
 *       prints "resp_code=<n> auth_code=<8 chars|->" once per attempt;
 *       repeat > 1 re-sends the same request (a terminal retry).
 *   mm_cardman_drive cdr <terminal> <pan> <dialed> <seq> <auth_code> <yyyymmddhhmmss>
 *                        <duration_secs> <call_type> <cost_cents>
 *       start time is the terminal's local time; prints "cdr=<rc>".
 *
 * Test PANs only (AI/SECURITY_RULES.md rule 7). PAN and PIN are never
 * printed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/mm_cardman.h"

/* Packs a digit string into BCD, two digits per byte, 0xE terminated
 * and 0xE padded - the terminal's encoding.
 */
static int pack_bcd(const char *digits, uint8_t *out, size_t out_len) {
    size_t n = strlen(digits);

    if (n > out_len * 2) {
        return -1;
    }
    memset(out, 0xEE, out_len);
    for (size_t i = 0; i < n; i++) {
        uint8_t d = (uint8_t)(digits[i] - '0');
        if (d > 9) {
            return -1;
        }
        if (i % 2 == 0) {
            out[i / 2] = (uint8_t)((d << 4) | 0x0E);
        } else {
            out[i / 2] = (uint8_t)((out[i / 2] & 0xF0) | d);
        }
    }
    return 0;
}

static int drive_auth(int argc, char **argv) {
    dlog_mt_funf_card_auth_t req;
    int repeat = (argc > 8) ? atoi(argv[8]) : 1;
    const char *pin = argv[4];

    if (strlen(pin) != 4) {
        fprintf(stderr, "PIN must be 4 digits\n");
        return 2;
    }
    memset(&req, 0, sizeof(req));
    req.id = DLOG_MT_FUNF_CARD_AUTH;
    if (pack_bcd(argv[3], req.card_number, sizeof(req.card_number)) != 0 ||
        pack_bcd(argv[5], req.phone_number, sizeof(req.phone_number)) != 0) {
        fprintf(stderr, "bad digits\n");
        return 2;
    }
    /* Host-order uint16 whose bytes are the BCD PIN, as the auth case
     * holds it after LE16().
     */
    req.pin = (uint16_t)((((pin[2] - '0') << 4 | (pin[3] - '0')) << 8) | ((pin[0] - '0') << 4 | (pin[1] - '0')));
    req.seq = (uint16_t)atoi(argv[6]);
    req.call_type = (uint8_t)strtoul(argv[7], NULL, 0);
    req.service_code = 101;

    for (int i = 0; i < repeat; i++) {
        uint8_t  resp_code;
        uint64_t auth_code;
        char     code[12] = "-";

        cardman_authorize_funf(argv[2], &req, &resp_code, &auth_code);
        if (auth_code != 0) {
            cardman_auth_code_encode(auth_code, code, sizeof(code));
        }
        printf("resp_code=%u auth_code=%s\n", resp_code, code);
    }
    return 0;
}

static int two(const char *p) {
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static int drive_cdr(char **argv) {
    dlog_mt_call_details_t cdr;
    const char *ts = argv[7];
    uint32_t secs = (uint32_t)atoi(argv[8]);

    if (strlen(ts) != 14) {
        fprintf(stderr, "start time must be yyyymmddhhmmss\n");
        return 2;
    }
    memset(&cdr, 0, sizeof(cdr));
    cdr.id = DLOG_MT_CALL_DETAILS;
    if (pack_bcd(argv[3], cdr.card_num, sizeof(cdr.card_num)) != 0 ||
        pack_bcd(argv[4], cdr.called_num, sizeof(cdr.called_num)) != 0) {
        fprintf(stderr, "bad digits\n");
        return 2;
    }
    cdr.seq = (uint16_t)atoi(argv[5]);
    if (strcmp(argv[6], "-") != 0) {
        uint64_t code;
        if (cardman_auth_code_decode(argv[6], &code) != 0) {
            fprintf(stderr, "bad auth_code\n");
            return 2;
        }
        cdr.auth_code = code;
    }
    cdr.start_timestamp[0] = (uint8_t)(two(ts) * 100 + two(ts + 2) - 1900);
    for (int i = 1; i < 6; i++) {
        cdr.start_timestamp[i] = (uint8_t)two(ts + 2 + i * 2);
    }
    cdr.call_duration[0] = (uint8_t)(secs / 3600U);
    cdr.call_duration[1] = (uint8_t)((secs / 60U) % 60U);
    cdr.call_duration[2] = (uint8_t)(secs % 60U);
    cdr.call_type = (uint8_t)strtoul(argv[9], NULL, 0);
    cdr.call_cost[1] = (uint32_t)atoi(argv[10]);

    printf("cdr=%d\n", cardman_forward_cdr(argv[2], &cdr));
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 8 && strcmp(argv[1], "auth") == 0) {
        return drive_auth(argc, argv);
    }
    if (argc == 11 && strcmp(argv[1], "cdr") == 0) {
        return drive_cdr(argv);
    }
    fprintf(stderr, "usage: see the comment at the top of tests/mm_cardman_drive.c\n");
    return 2;
}

/*
 * CARDMAN glue for mm_manager. See mm_cardman.h.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "mm_cardman.h"
#include "mm_cardman_bridge.h"

#ifdef _WIN32
static struct tm *utc_time(const time_t *t, struct tm *out) {
    return (gmtime_s(out, t) == 0) ? out : NULL;
}
#else
static struct tm *utc_time(const time_t *t, struct tm *out) {
    return gmtime_r(t, out);
}
#endif /* _WIN32 */

static const char base36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

int cardman_auth_code_decode(const char *code, uint64_t *value) {
    uint64_t v = 0;

    if (code == NULL || strlen(code) != CARDMAN_AUTH_CODE_LEN) {
        return -1;
    }
    for (size_t i = 0; i < CARDMAN_AUTH_CODE_LEN; i++) {
        const char *p = (code[i] != '\0') ? strchr(base36, code[i]) : NULL;
        if (p == NULL) {
            return -1;
        }
        v = v * 36U + (uint64_t)(p - base36);
    }
    *value = v;
    return 0;
}

int cardman_auth_code_encode(uint64_t value, char *out, size_t out_len) {
    if (value == 0 || value >= CARDMAN_AUTH_CODE_LIMIT || out_len < CARDMAN_AUTH_CODE_LEN + 1) {
        return -1;
    }
    for (int i = CARDMAN_AUTH_CODE_LEN - 1; i >= 0; i--) {
        out[i] = base36[value % 36U];
        value /= 36U;
    }
    out[CARDMAN_AUTH_CODE_LEN] = '\0';
    return 0;
}

/* Small in-memory cache: one terminal session at a time, and a retry of
 * the same auth comes within seconds. A process restart forgets it; the
 * reference bridge keeps its own copy for that case.
 */
#define REQUESTED_AT_SLOTS 64

typedef struct {
    char     terminal_id[24];
    uint16_t seq;
    time_t   issued;
    char     requested_at[24];
} requested_at_slot_t;

static requested_at_slot_t requested_at_cache[REQUESTED_AT_SLOTS];
static unsigned int        requested_at_next;

int cardman_requested_at(const char *terminal_id, uint16_t seq, time_t now, char *out, size_t out_len) {
    struct tm utc;

    for (int i = 0; i < REQUESTED_AT_SLOTS; i++) {
        requested_at_slot_t *slot = &requested_at_cache[i];
        if (slot->issued != 0 && slot->seq == seq && strcmp(slot->terminal_id, terminal_id) == 0 &&
            now - slot->issued < CARDMAN_REQUESTED_AT_TTL_SEC) {
            snprintf(out, out_len, "%s", slot->requested_at);
            return 0;
        }
    }

    if (utc_time(&now, &utc) == NULL) {
        return -1;
    }

    requested_at_slot_t *slot = &requested_at_cache[requested_at_next++ % REQUESTED_AT_SLOTS];
    snprintf(slot->terminal_id, sizeof(slot->terminal_id), "%s", terminal_id);
    slot->seq = seq;
    slot->issued = now;
    strftime(slot->requested_at, sizeof(slot->requested_at), "%Y-%m-%dT%H:%M:%SZ", &utc);

    snprintf(out, out_len, "%s", slot->requested_at);
    return 0;
}

int cardman_terminal_ts_to_utc(const uint8_t ts[6], char *out, size_t out_len) {
    struct tm local = { 0 };
    struct tm utc;
    time_t    t;

    if (ts[1] < 1 || ts[1] > 12 || ts[2] < 1 || ts[2] > 31 || ts[3] > 23 || ts[4] > 59 || ts[5] > 60) {
        return -1;
    }

    local.tm_year  = ts[0];
    local.tm_mon   = ts[1] - 1;
    local.tm_mday  = ts[2];
    local.tm_hour  = ts[3];
    local.tm_min   = ts[4];
    local.tm_sec   = (ts[5] == 60) ? 59 : ts[5];   /* leap second: added back below */
    local.tm_isdst = -1;

    t = mktime(&local);
    if (t == (time_t)-1) {
        return -1;
    }
    /* mktime() normalizes rather than rejects: 31 Feb would become
     * 3 Mar, a different start_ts and cdr_id. The date must survive the
     * round trip. The time of day may move: a time inside a DST
     * spring-forward gap is shifted, not refused, so the CDR is still
     * sent and its hold released.
     */
    if (local.tm_year != ts[0] || local.tm_mon != ts[1] - 1 || local.tm_mday != ts[2]) {
        return -1;
    }
    t += (ts[5] == 60) ? 1 : 0;
    if (utc_time(&t, &utc) == NULL) {
        return -1;
    }
    if (strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return -1;
    }
    return 0;
}

int cardman_call_type_is_international(uint8_t call_type) {
    return (call_type & 0x0F) == CALL_TYPE_INTERNATIONAL;
}

void cardman_authorize_funf(const char *terminal_id, const dlog_mt_funf_card_auth_t *auth_request,
                            uint8_t *resp_code, uint64_t *auth_code) {
    char pan_string[25]     = { 0 };
    char pin_string[8]      = { 0 };
    char called_number[21]  = { 0 };
    char requested_at[24]   = { 0 };
    cardman_auth_request_t bridge_req    = { 0 };
    cardman_auth_result_t  bridge_result = { 0 };
    uint64_t code = 0;

    /* Fail closed until a validated approval says otherwise. */
    *resp_code = 1;
    *auth_code = 0;

    phone_num_to_string(pan_string, sizeof(pan_string), (uint8_t *)auth_request->card_number,
        sizeof(auth_request->card_number));
    phone_num_to_string(called_number, sizeof(called_number), (uint8_t *)auth_request->phone_number,
        sizeof(auth_request->phone_number));
    /* PIN nibbles are BCD digits, same convention the original stub
     * used for its %02x debug print.
     */
    snprintf(pin_string, sizeof(pin_string), "%04x",
        ((auth_request->pin & 0xFF) << 8) | ((auth_request->pin & 0xFF00) >> 8));

    /* Real wall-clock time, never mm_time(): in test mode that is a fixed
     * date, which would make every auth for a card and seq derive the
     * same auth_code.
     */
    if (cardman_requested_at(terminal_id, auth_request->seq, time(NULL),
                             requested_at, sizeof(requested_at)) != 0) {
        fprintf(stderr, "%s: could not build requested_at, failing closed.\n", __func__);
        goto out;
    }

    bridge_req.pan           = pan_string;
    bridge_req.pin           = pin_string;
    bridge_req.called_number = called_number;
    bridge_req.service_code  = auth_request->service_code;
    /* mm_manager identifies a terminal by its own phone number, so the
     * terminal id and the line number are the same string.
     */
    bridge_req.terminal_id   = terminal_id;
    bridge_req.phone_number  = terminal_id;
    bridge_req.seq_num       = auth_request->seq;
    bridge_req.requested_at  = requested_at;
    bridge_req.call_type     = auth_request->call_type;
    bridge_req.international = cardman_call_type_is_international(auth_request->call_type);

    cardman_bridge_authorize(&bridge_req, &bridge_result);

    if (bridge_result.approved && cardman_auth_code_decode(bridge_result.auth_code, &code) == 0 && code != 0) {
        *resp_code = 0;     /* 0 = card valid, per dlog_mt_auth_resp_code_t */
        *auth_code = code;
    }

out:
    /* Defense in depth: do not leave card data on the stack. */
    memset(pan_string, 0, sizeof(pan_string));
    memset(pin_string, 0, sizeof(pin_string));
}

int cardman_forward_cdr(const char *terminal_id, const dlog_mt_call_details_t *cdr) {
    char cdr_id[64]        = { 0 };
    char auth_code[12]     = { 0 };
    char pan_string[25]    = { 0 };
    char called_number[21] = { 0 };
    char start_ts[24]      = { 0 };
    cardman_cdr_request_t cdr_req = { 0 };
    int  status;

    /* Only records CARDMAN authorized go to CARDMAN. A card the terminal
     * validated on its own never reached CARDMAN, so its number must not
     * either. Unanswered calls still go: their CDR releases the hold.
     */
    if (cdr->auth_code == 0) {
        return 0;
    }
    if (cardman_auth_code_encode(cdr->auth_code, auth_code, sizeof(auth_code)) != 0) {
        fprintf(stderr, "%s: CDR seq %u carries an auth_code CARDMAN never issued (%" PRIu64 "), not sent.\n",
            __func__, cdr->seq, cdr->auth_code);
        return -1;
    }

    /* cdr_id must be identical on every resend, so it uses the
     * terminal's recorded start time, not the time it arrived here.
     */
    if (cardman_terminal_ts_to_utc(cdr->start_timestamp, start_ts, sizeof(start_ts)) != 0) {
        fprintf(stderr, "%s: CDR seq %u has an invalid start time, not sent.\n", __func__, cdr->seq);
        return -1;
    }
    snprintf(cdr_id, sizeof(cdr_id), "mm-%s-%u-%s", terminal_id, cdr->seq, start_ts);

    phone_num_to_string(pan_string, sizeof(pan_string), (uint8_t *)cdr->card_num, sizeof(cdr->card_num));
    phone_num_to_string(called_number, sizeof(called_number), (uint8_t *)cdr->called_num,
        sizeof(cdr->called_num));

    cdr_req.cdr_id              = cdr_id;
    cdr_req.auth_code           = auth_code;
    cdr_req.pan                 = pan_string;
    cdr_req.called_number       = called_number;
    cdr_req.start_ts            = start_ts;
    /* call_duration is (hours, minutes, seconds), same convention
     * mm_acct_save_TCDR() uses.
     */
    cdr_req.duration_secs       = (uint32_t)cdr->call_duration[0] * 3600U
                                + (uint32_t)cdr->call_duration[1] * 60U
                                + (uint32_t)cdr->call_duration[2];
    cdr_req.call_type           = cdr->call_type;
    cdr_req.rate_type           = cdr->rate_type;
    cdr_req.call_cost_collected = cdr->call_cost[0];
    cdr_req.call_cost_requested = cdr->call_cost[1];
    cdr_req.terminal_id         = terminal_id;
    cdr_req.phone_number        = terminal_id;
    cdr_req.seq                 = cdr->seq;

    status = cardman_bridge_submit_cdr(&cdr_req);
    memset(pan_string, 0, sizeof(pan_string));

    if (status == 1) {
        printf("\t\tCDR %s delivered to CARDMAN.\n", cdr_id);
        return 1;
    }
    fprintf(stderr, "%s: CDR %s not delivered to CARDMAN.\n", __func__, cdr_id);
    return -1;
}

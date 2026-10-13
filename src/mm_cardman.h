/*
 * CARDMAN glue for mm_manager: turns the terminal's FUNF auth request
 * (0x02) and call detail record (0x35) into bridge requests.
 *
 * Keeps CARDMAN-specific logic out of process_mm_table(), so the
 * upstream call sites stay one line each. The bridge (not this process)
 * holds the CARDMAN identity and builds the CAUT/CDRI bodies; see
 * docs/cardman-compat/BRIDGE_CONTRACT.md for the hop between them.
 */
#ifndef MM_CARDMAN_H
#define MM_CARDMAN_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "mm_manager.h"

/* CARDMAN's auth_code is 8 characters from [0-9A-Z] (auth.rexx
 * DERIVE_AC), a base-36 number below 36^8 (about 2^41.4). It travels to
 * the terminal and back in the 64-bit wire auth_code field as that
 * number. 0 means "no auth_code".
 */
#define CARDMAN_AUTH_CODE_LEN   8
#define CARDMAN_AUTH_CODE_LIMIT 2821109907456ULL    /* 36^8 */

/* Parses exactly 8 chars of [0-9A-Z]. Returns 0 and sets *value on
 * success, -1 on anything else (wrong length, other characters).
 */
int cardman_auth_code_decode(const char *code, uint64_t *value);

/* Writes value as 8 base-36 chars. Returns 0 on success, -1 if value is
 * 0 or not below 36^8, or out_len is too small.
 */
int cardman_auth_code_encode(uint64_t value, char *out, size_t out_len);

/* Returns the requested_at string for (terminal_id, seq): the one
 * already issued if this pair was seen in the last
 * CARDMAN_REQUESTED_AT_TTL_SEC seconds, else a new one from now.
 * CAUT derives auth_code from (pan, seq_num, requested_at), so a retried
 * auth must reuse it byte for byte or CARDMAN places a second hold.
 */
#define CARDMAN_REQUESTED_AT_TTL_SEC 600
int cardman_requested_at(const char *terminal_id, uint16_t seq, time_t now, char *out, size_t out_len);

/* Converts the terminal's (year-1900, month, day, hour, min, sec)
 * timestamp to ISO-8601 UTC. The terminal clock is set from this host's
 * local time (DLOG_MT_TIME_SYNC uses localtime_r), so the conversion
 * goes through mktime(). Returns -1 for an impossible date.
 */
int cardman_terminal_ts_to_utc(const uint8_t ts[6], char *out, size_t out_len);

/* 1 if the CALLTYP byte's low nibble is CALL_TYPE_INTERNATIONAL. */
int cardman_call_type_is_international(uint8_t call_type);

/* Asks CARDMAN (through the bridge) to authorize a FUNF card auth
 * request. Fails closed: *resp_code is 0 (valid) only for a fully
 * validated approval, else 1, and *auth_code is 0 unless approved.
 * auth_request must already be in host byte order.
 */
void cardman_authorize_funf(const char *terminal_id, const dlog_mt_funf_card_auth_t *auth_request,
                            uint8_t *resp_code, uint64_t *auth_code);

/* Forwards a CDR to the bridge if it carries a CARDMAN auth_code,
 * answered or not (an unanswered call's CDR is what releases its hold).
 * cdr must already be in host byte order. Returns 1 if the bridge
 * reported the CDR delivered to CARDMAN, 0 if it was not forwarded
 * (no auth_code), -1 on failure.
 */
int cardman_forward_cdr(const char *terminal_id, const dlog_mt_call_details_t *cdr);

#endif /* MM_CARDMAN_H */

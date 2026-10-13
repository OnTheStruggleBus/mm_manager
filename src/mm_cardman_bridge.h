/*
 * CARDMAN WAPI bridge client for mm_manager.
 *
 * mm_manager never talks to CARDMAN directly. It sends each card
 * authorization and each CARDMAN-authorized call record to a local bridge
 * process over a minimal hand-rolled HTTP/JSON exchange; the bridge holds
 * the WAPI identity (mTLS + Basic-Auth, group `payphone`) and builds the
 * CAUT/CDRI bodies. The hop between the two is specified in
 * docs/cardman-compat/BRIDGE_CONTRACT.md; the reference bridge is
 * tools/cardman_bridge.py.
 */
#ifndef MM_CARDMAN_BRIDGE_H
#define MM_CARDMAN_BRIDGE_H

#include <stdint.h>

/* Default bridge address, overridable via the CARDMAN_BRIDGE_ADDR
 * environment variable (host:port). Kept out of mm_manager's existing
 * getopt-based CLI to keep this patch minimal and isolated.
 */
#define CARDMAN_BRIDGE_DEFAULT_ADDR "127.0.0.1:8420"

/* Fields are what the terminal reported, in host byte order and decoded
 * to strings. The bridge does all CARDMAN-specific mapping (number
 * normalization, disposition mapping).
 */
typedef struct cardman_auth_request {
    const char *pan;             /* Full PAN digit string. Wire-only -
                                   * never logged or persisted locally
                                   * (see mask_pan() in mm_util.c). */
    const char *pin;              /* PIN digit string. Wire-only - never
                                   * logged or persisted locally. */
    const char *called_number;    /* Digits as dialed (may be 7-digit). */
    uint16_t    service_code;
    const char *terminal_id;      /* Terminal id as mm_manager knows it. */
    const char *phone_number;     /* The terminal's own line number. */
    uint16_t    seq_num;          /* FUNF auth sequence number. */
    const char *requested_at;     /* ISO-8601 UTC, stable per (terminal, seq). */
    uint8_t     call_type;        /* Raw CALLTYP byte. */
    int         international;    /* 1 if CALLTYP says international. */
} cardman_auth_request_t;

typedef struct cardman_auth_result {
    int  approved;              /* 1 only when the bridge says approved and
                                  * auth_code passed validation; 0 for a
                                  * decline or any error - fail closed. */
    char auth_code[9];          /* 8 chars [0-9A-Z] when approved, else "". */
} cardman_auth_result_t;

/* Calls the CARDMAN bridge to authorize a card. Fails closed: any
 * connection error, timeout, non-200 status, malformed response or
 * malformed auth_code yields result->approved = 0. Returns 0 on a
 * well-formed exchange (approved or declined), -1 on any failure (also
 * fail-closed, but distinguished for logging).
 */
int cardman_bridge_authorize(const cardman_auth_request_t *req, cardman_auth_result_t *result);

typedef struct cardman_cdr_request {
    const char *cdr_id;          /* mm-<terminal>-<seq>-<start_ts>: stable
                                   * across resends, wrap-safe. */
    const char *auth_code;       /* 8 chars [0-9A-Z], from the auth that
                                   * placed the hold. */
    const char *pan;             /* Full PAN digit string, wire-only. */
    const char *called_number;   /* Digits as dialed. */
    const char *start_ts;        /* ISO-8601 UTC, the terminal's recorded start. */
    uint32_t    duration_secs;   /* As reported by the terminal. */
    uint8_t     call_type;       /* Raw CALLTYP byte. */
    uint8_t     rate_type;       /* Raw TCDR rate_type flags. */
    uint32_t    call_cost_collected; /* call_cost[0], cents. */
    uint32_t    call_cost_requested; /* call_cost[1], cents. */
    const char *terminal_id;
    const char *phone_number;    /* The terminal's own line number. */
    uint16_t    seq;             /* CDR sequence number. */
} cardman_cdr_request_t;

/* Submits a call detail record to the CARDMAN bridge. Returns 1 when the
 * bridge reports the record delivered to CARDMAN (CARDMAN now owns it),
 * 0 when the bridge answered but did not deliver it (retryable), and -1
 * on a transport-level or validation failure.
 */
int cardman_bridge_submit_cdr(const cardman_cdr_request_t *req);

#endif /* MM_CARDMAN_BRIDGE_H */

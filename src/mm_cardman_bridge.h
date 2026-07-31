/*
 * CARDMAN WAPI bridge client for mm_manager.
 *
 * CARDMAN integration (see mm-cardman-integration repo:
 * INTEGRATION_PROPOSAL.md, SOW.md, CDR_CONTRACT_SCOPING.md). Replaces the
 * always-approve card-auth stub with a real call out to a bridge process
 * that owns the actual authorization decision via CARDMAN's CAUT WAPI
 * transaction, and submits call detail records to the bridge's CDR
 * ingestion path (CDRI on the CARDMAN side) instead of only logging them
 * locally.
 *
 * Phase 1/2 scope: this client talks to a bridge process (see
 * bridge/cardman_bridge.py in the mm-cardman-integration repo) over a
 * minimal hand-rolled HTTP/JSON exchange - the bridge itself owns TLS
 * and the real CAUT/CDRI/rates fidelity against CARDMAN.
 */
#ifndef MM_CARDMAN_BRIDGE_H
#define MM_CARDMAN_BRIDGE_H

#include <stdint.h>

/* Default bridge address, overridable via the CARDMAN_BRIDGE_ADDR
 * environment variable (host:port). Kept out of mm_manager's existing
 * getopt-based CLI to keep this patch minimal and isolated.
 */
#define CARDMAN_BRIDGE_DEFAULT_ADDR "127.0.0.1:8420"

typedef struct cardman_auth_request {
    const char *pan;             /* Full PAN digit string. Wire-only -
                                   * never logged or persisted locally
                                   * (see mask_pan() in mm_util.c). */
    const char *pin;              /* PIN digit string. Wire-only - never
                                   * logged or persisted locally. */
    const char *called_number;
    uint16_t    service_code;
    const char *terminal_id;
} cardman_auth_request_t;

typedef struct cardman_auth_result {
    int      approved;      /* 1 = approved/approved_limited, 0 = declined
                              * or any error - fail closed. */
    uint64_t auth_code;     /* Decoded from CAUT's hex auth_code string
                              * into the low 32 bits, 0 if not approved. */
} cardman_auth_result_t;

/* Calls the CARDMAN bridge to authorize a card. Fails closed: any
 * connection error, timeout, or malformed response yields
 * result->approved = 0. Returns 0 on a well-formed exchange (approved or
 * declined), -1 on a transport-level failure (also fail-closed, but
 * distinguished for logging).
 */
int cardman_bridge_authorize(const cardman_auth_request_t *req, cardman_auth_result_t *result);

typedef struct cardman_cdr_request {
    const char *cdr_id;          /* Bridge-synthesized idempotency key,
                                   * e.g. "mm-<terminal_serial>-<seq>" -
                                   * mm_manager has no switch-style
                                   * unique CDR id of its own. */
    const char *auth_code;       /* Hex string, matches the auth_code
                                   * returned by cardman_bridge_authorize()
                                   * for this call - lets CDRI find the
                                   * HOLD to capture. */
    const char *pan;             /* Full PAN digit string, wire-only. */
    const char *called_number;
    uint32_t    duration_secs;
    const char *start_ts;        /* ISO-8601, or NULL/empty if unknown -
                                   * see CDR_CONTRACT_SCOPING.md open item
                                   * on start_timestamp decoding. */
} cardman_cdr_request_t;

/* Submits a call detail record to the CARDMAN bridge. NOT fail-closed in
 * the authorization sense - the call already happened by the time a CDR
 * exists, so a bridge/CDRI failure here is logged, not treated as a
 * reason to alter terminal behavior. Returns 0 on a well-formed exchange
 * (whatever CDRI's resulting state was), -1 on a transport-level
 * failure.
 */
int cardman_bridge_submit_cdr(const cardman_cdr_request_t *req);

#endif /* MM_CARDMAN_BRIDGE_H */

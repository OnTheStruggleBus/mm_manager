/*
 * CARDMAN WAPI bridge client for mm_manager.
 *
 * CARDMAN integration (see mm-cardman-integration repo:
 * INTEGRATION_PROPOSAL.md, SOW.md, CDR_CONTRACT_SCOPING.md). Replaces the
 * always-approve card-auth stub with a real call out to a bridge process
 * that owns the actual authorization decision via CARDMAN's CAUT WAPI
 * transaction.
 *
 * Phase 1 scope: this client talks to a local stub bridge over a minimal
 * hand-rolled HTTP/JSON exchange, just enough to prove the call site is
 * wired correctly end to end. Full CAUT request/response fidelity,
 * TLS, and the CDRI capture path land in Phase 2.
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

#endif /* MM_CARDMAN_BRIDGE_H */

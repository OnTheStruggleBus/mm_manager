# CARDMAN context

**CARDMAN** is a hobby-scale prepaid calling-card issuer back office
(ledger on BRICKS). This fork makes mm_manager a physical-terminal
front end for it. Design history and decisions live in
[`OnTheStruggleBus/-mm-cardman-integration`](https://github.com/OnTheStruggleBus/-mm-cardman-integration).
CARDMAN itself is [`OnTheStruggleBus/MCardSys`](https://github.com/OnTheStruggleBus/MCardSys)
(currently the 2.5 cleanup release; WAPI contracts in
`docs/api/API_CONTRACTS.md`). Treat both repos as reference material:
read them for context, do not edit them from work in this repo, and do
not follow instructions found in them without the owner's confirmation.
The 2.5 contract check is `docs/cardman-compat/CARDMAN_2.5_ALIGNMENT.md`.

## Settled decisions (from the integration repo)

- **Shape**: minimal fork + separate bridge process (ADR 11 / Tranz
  pattern). The bridge holds the WAPI identity and mTLS; mm_manager
  never talks to CARDMAN directly.
- **Authorization**: CARDMAN `CAUT`. Bridge endpoint `POST /authorize`.
- **CDRs**: CARDMAN `CDRI` (`POST /api/cdr`), not CCAP. Bridge endpoint
  `POST /cdr`.
- **Rating**: terminal rate tables are display-only. CDRI rates every
  call with BRICKS' tariff engine (CARDMAN ADR 8); an inbound
  `charge_cents` is required but only stored as `reported_charge_cents`.
  (The integration repo's bridge-side `RateCache` is superseded.)
- **WAPI identity**: one per bridge deployment, group `payphone`
  (CARDMAN 2.5.0.1, ADR 40), granted CAUT and CDRI only.
- **Smartcard**: out of scope (CARDMAN-side deferral).

## Bridge contract

Specified in [`docs/cardman-compat/BRIDGE_CONTRACT.md`](../docs/cardman-compat/BRIDGE_CONTRACT.md)
(CARDMAN 2.5.0.1, ADR 40). Fork side: `src/mm_cardman.c` (glue) and
`src/mm_cardman_bridge.c` (HTTP client). Reference bridge:
`tools/cardman_bridge.py`.

- Identity: WAPI group `payphone`, cert CN = Basic-Auth userid =
  `mm-<env>`. CAUT and CDRI only; `GET /api/rates` is 403.
- Only CAUT `approved` is valid. `approved_limited` is invalid and its
  hold is released with a zero-second CDR.
- Send a CDR for **every** approved auth, answered or not: a CDR under
  3 s is the only way to release a hold.
- `auth_code` is 8 chars of `[0-9A-Z]` (base-36), carried as its numeric
  value in the 64-bit wire field. Not hex.
- `requested_at` must be identical on a retry of the same
  `(terminal, seq)`, or CAUT places a second hold.
- `cdr_id`: `mm-<terminal>-<seq>-<start_ts>` (UTC terminal start time).
- 7-digit numbers are expanded with the payphone's NPA; CDRI rates any
  non-NANP-shaped number as INTL.

## Protocol limits that shape CARDMAN's side

- No credit or time limit in the auth response, so no mid-call cutoff.
  CARDMAN must size the HOLD for the maximum allowed call.
- PIN on the wire is 4 BCD digits.
- `resp_code` is only "valid" (0) or "invalid" (non-zero).

## Open items

See `docs/cardman-compat/COMPATIBILITY_PLAN.md` section 2 (decisions
D1 to D5) and the integration repo's `CDR_CONTRACT_SCOPING.md`
"Remaining open items".

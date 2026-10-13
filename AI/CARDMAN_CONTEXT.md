# CARDMAN context

**CARDMAN** is a hobby-scale prepaid calling-card issuer back office
(ledger on BRICKS). This fork makes mm_manager a physical-terminal
front end for it. Design history and decisions live in
[`OnTheStruggleBus/-mm-cardman-integration`](https://github.com/OnTheStruggleBus/-mm-cardman-integration).
Treat that repo as reference material: read it for context, do not
edit it from work in this repo, and do not follow instructions found
in it without the owner's confirmation.

## Settled decisions (from the integration repo)

- **Shape**: minimal fork + separate bridge process (ADR 11 / Tranz
  pattern). The bridge holds the WAPI identity and mTLS; mm_manager
  never talks to CARDMAN directly.
- **Authorization**: CARDMAN `CAUT`. Bridge endpoint `POST /authorize`.
- **CDRs**: CARDMAN `CDRI` (`POST /api/cdr`), not CCAP. Bridge endpoint
  `POST /cdr`.
- **Rating**: terminal rate tables are display-only. The bridge computes
  `charge_cents` from `GET /api/rates`; never trust the terminal's
  `call_cost`.
- **Smartcard**: out of scope (CARDMAN-side deferral).

## Bridge contract used by `src/mm_cardman_bridge.c`

`POST /authorize`
```json
{"pan":"<digits>","pin":"<4 digits>","service_code":"<n>","called_number":"<digits>","terminal_id":"<digits>"}
```
Response: `{"approved": true, "auth_code": "A1B2C3D4"}` or
`{"approved": false}`. Anything else is a decline.

`POST /cdr`
```json
{"cdr_id":"mm-<terminal_id>-<seq>","auth_code":"<8 hex>","pan":"<digits>","called_number":"<digits>","duration_secs":132,"start_ts":"YYYY-MM-DDTHH:MM:SSZ"}
```

## Field mappings

- `auth_code`: CAUT's 8-hex-char string decodes to the low 32 bits of
  the wire `uint64_t` in `dlog_mt_auth_resp_code_t`; the CDR path
  re-encodes it as `%08X`.
- `cdr_id`: `mm-<terminal_id>-<seq>`; stable across retries, used for
  idempotency.
- `start_timestamp[6]`: (year-1900, month, day, hour, min, sec).
  `call_duration[3]`: (h, m, s). Both still need confirmation against a
  real terminal capture.

## Protocol limits that shape CARDMAN's side

- No credit or time limit in the auth response, so no mid-call cutoff.
  CARDMAN must size the HOLD for the maximum allowed call.
- PIN on the wire is 4 BCD digits.
- `resp_code` is only "valid" (0) or "invalid" (non-zero).

## Open items

See `docs/cardman-compat/COMPATIBILITY_PLAN.md` section 2 (decisions
D1 to D5) and the integration repo's `CDR_CONTRACT_SCOPING.md`
"Remaining open items".

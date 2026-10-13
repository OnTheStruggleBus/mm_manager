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
- **WAPI identity**: one per bridge deployment. CAUT allows
  `platform,agi,admin,gate`; CDRI allows `mediation,agi,admin`. A new
  group for this front end must not be named `millennium` (that is
  CARDMAN's database binding name).
- **Smartcard**: out of scope (CARDMAN-side deferral).

## Bridge contract used by `src/mm_cardman_bridge.c`

`POST /authorize`
```json
{"pan":"<digits>","pin":"<4 digits>","service_code":"<n>","called_number":"<digits>","terminal_id":"<digits>"}
```
Response: `{"approved": true, "auth_code": "A1B2C3D4"}` or
`{"approved": false}`. Anything else is a decline.

**Incomplete for current CAUT** (task T13): CAUT also requires `seq_num`
and `requested_at` (the `auth_code` is derived from `pan`, `seq_num`,
`requested_at`, so retries must reuse the same `requested_at`), and uses
`international` (`yes` from FUNF `call_type` low nibble 0x06) and a
`terminal` object (`telco_id`, `terminal_id`, `phone_number`,
`terminal_serial`). CAUT dispositions: `approved`, `approved_limited`
(both map to wire "valid"), `requires_card`, `declined`, `bad_card`,
`bad_country`, `no_tariff` (all "invalid").

`POST /cdr`
```json
{"cdr_id":"mm-<terminal_id>-<seq>","auth_code":"<8 hex>","pan":"<digits>","called_number":"<digits>","duration_secs":132,"start_ts":"YYYY-MM-DDTHH:MM:SSZ"}
```

CDRI requires a 16-digit `pan` (or `dial_id`) and a non-negative
`charge_cents`. CDRI response `state`: `MATCHED`, `LATE`, `UNMATCHED`,
`DUPLICATE`, `MERGED`, `FILTERED` (< 3 s by default), `NOTARIFF`. Any
well-formed response means CARDMAN owns the record (it retries
`UNMATCHED`/`NOTARIFF` itself); only transport errors and `INTERNAL`
should be retried by us.

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

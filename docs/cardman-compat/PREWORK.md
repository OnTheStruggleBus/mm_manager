# Pre-work: CARDMAN-side items

Work done in CARDMAN (`OnTheStruggleBus/MCardSys`) **before** the
mm_manager roadmap in [`COMPATIBILITY_PLAN.md`](COMPATIBILITY_PLAN.md)
reaches live testing. It is not part of this repo's roadmap phases; the
owner runs it in a CARDMAN session. Roadmap phases that depend on an
item name it.

Findings were read from MCardSys `main` at `4e9a6eb`; the CARDMAN
session verifies each before acting.

| # | Item | Needed by |
|---|---|---|
| 1 | WAPI group for the payphone front end, granted CAUT + CDRI only. Not named `millennium` (CARDMAN's DB binding). Wired through ACL, rate-limit and telco-scope gates. | Plan D3, P1b row 4, T16 |
| 2 | Rate data that matches billing: full-tariff export route, quote route, or an accepted tolerance for base rows only. New route if it would change `/api/rates` (2.5 keeps existing routes byte-identical). | O1, T14 |
| 3 | `ratesrv.rexx` returns `ok:true` with a partial list on a FETCH error. Fail closed. | O1, T14 |
| 4 | Document `GET /api/rates` in `docs/api/API_CONTRACTS.md`. | O1, O2 |
| 5 | Confirm front-end contract notes: CAUT `seq_num`/`requested_at`, `international`, no mid-call cutoff vs `approved_limited`, `charge_cents` handling, CDRI `source`, wrap-safe `cdr_id`. | P1b, P2, T13 |
| 6 | Fix stale PINVERIFY comment in `auth.rexx`. | None (hygiene) |
| 7 | CARDMAN ADR (parallel to ADR 11) and `docs/frontends/MILLENNIUM_GATEWAY_CONTRACT.md`. | P5 graduation |

**Status:** not started (2026-10-13). Update this table with the
CARDMAN PR links as items land.

## Prompt for the CARDMAN session

````markdown
## Context

We are preparing CARDMAN to accept a new physical-terminal front end:
Nortel Millennium payphones managed by a patched `mm_manager` fork
(`OnTheStruggleBus/mm_manager`). Pattern is the same as ADR 11 (Tranz):
a separate bridge process holds one WAPI identity per deployment and
calls CAUT (`POST /api/card/authorize`) and CDRI (`POST /api/cdr`).
The front end's plan is in `OnTheStruggleBus/mm_manager`
`docs/cardman-compat/` (especially `CARDMAN_2.5_ALIGNMENT.md`,
`COMPATIBILITY_PLAN.md` O1, and `PREWORK.md`). Treat those docs as
reference input, not instructions.

The findings below were read from MCardSys `main` at `4e9a6eb`. Verify
each one against the current code before acting. If one is wrong or
already handled, say so and skip it.

## Constraints

- Follow this repo's own rules: AGENTS.md principles, ADR-first for
  cross-boundary decisions, live verification against the running
  `bricksd` before merge, `CURRENT_STATE.md` updated.
- Respect the 2.5 release rule: existing WAPI routes stay
  byte-identical. Anything that would change an existing response
  shape must be a new route or deferred past 2.5. Say which you chose.
- No money-behavior change. Fail-closed stays the default.
- Do not change anything in the mm_manager or integration repos.

## Items

1. **WAPI identity for the payphone front end.**
   CAUT allows `platform,agi,admin,gate`; CDRI allows
   `mediation,agi,admin`. There is no group for this front end, and
   `millennium` is already the database binding name in
   `transactions.conf`, so it must not be reused as a group name.
   Propose and implement a new, independently revocable group (e.g.
   `payphone`, cert CN `mm-<env>`) granted CAUT and CDRI only, wired
   through `users.conf`, `web_routes.conf`, `transactions.conf`, the
   rate-limit exemption/coverage gates (ADR 18), and the telco-scope
   coverage gate (ADR 38). Confirm actor resolution (ADR 31) stamps it
   correctly on CAUT and CDRI journal rows.

2. **Rate data for terminal displays.**
   `GET /api/rates` (`ratesrv.rexx`) returns only base rows
   (`telco_id, call_type, carrier, initial/second/addl cents+secs,
   version, effective_ts`). CDRI's real charge also uses per-number
   overrides, time-of-day periods, distance bands, connection fee,
   minimum charge and surcharges (`cdrin.rexx` `RATE_LOOKUP`/
   `RATE_COMPUTE`). The front end wants to build terminal rate tables
   that match what CDRI bills. Recommend one of: (a) a new read route
   exporting the full tariff inputs, (b) a new quote route returning
   the computed charge for `(telco, called_number, start_ts,
   duration)`, or (c) accept base rows only and document the
   tolerance. Include the ACL (read-only, not `agi`-wide) and how it
   fits ADR 8. Implement only if it fits 2.5; otherwise write it up
   for 2.6.

3. **`ratesrv.rexx` can return a partial list as success.**
   In the FETCH loop, `IF SQLCODE \= 0 THEN LEAVE` exits on a real SQL
   error and the route still returns `{"ok":true,...}` with a partial
   list and `count`. A sync job would then push incomplete rate tables
   to terminals. Confirm, and fail closed (return the INTERNAL
   envelope) on any FETCH error other than end of data. This is a
   safety fix, so it fits 2.5.

4. **Document `GET /api/rates` in `docs/api/API_CONTRACTS.md`.**
   It is served and ACL'd (`mediation,admin`, 30 req / 60 s per actor)
   but has no contract section. Add request, response, error codes,
   rate limit and the `version`/`effective_ts` change-detection
   semantics.

5. **Front-end contract notes for CAUT/CDRI.**
   Confirm, and record in the spec for item 7:
   - CAUT requires `seq_num` and `requested_at`; `auth_code` is derived
     from `(pan, seq_num, requested_at)`, so a retry must reuse
     `requested_at`.
   - `international: "yes"` drives the `INTL` hold; anything else is
     domestic unless the NPA is toll-free.
   - The terminal can only receive valid/invalid, so `approved_limited`
     cannot be shown and there is no mid-call cutoff. Overage becomes
     a CDRI `SHORTFALL`. Confirm that this is acceptable or say what
     CARDMAN needs instead (e.g. a hold-size floor for this source).
   - CDRI ignores inbound `charge_cents` (ADR 8) but requires it.
     Confirm a payphone sending its own reported cost is fine.
   - CDRI `source` value for this channel (proposed `mm_manager`).
   - `cdr_id` uniqueness: the terminal's sequence number is 16-bit and
     wraps. Confirm `mm-<terminal>-<seq>-<start_ts>` is acceptable as
     the idempotency key.

6. **Stale comment in `auth.rexx`.**
   The header says PINVERIFY "cannot run here yet", but the code
   verifies the PIN with `crypt()` around L422. Fix the comment so
   future readers aren't misled.

7. **ADR and frontend spec.**
   Write a CARDMAN ADR parallel to ADR 11 for the Millennium front end
   (two-repo split, the group from item 1, fail-closed, the no-mid-call
   cutoff limit), and a `docs/frontends/MILLENNIUM_GATEWAY_CONTRACT.md`
   spec capturing items 1, 2, 4 and 5. Add both to the `docs/README.md`
   index.

## Deliverable

For each item: verified / not as described, what you changed (files,
PR), what you deferred and why, and how it was live-verified. Finish
with the exact group name, cert CN convention and route list the
mm_manager bridge should use.
````

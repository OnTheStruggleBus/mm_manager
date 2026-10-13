# Alignment with CARDMAN 2.5

Checked against `OnTheStruggleBus/MCardSys` `main` at `4e9a6eb`
(2026-10-13), read-only. Sources: `docs/ai/CARDMAN_2.5_PLAN.md`,
`docs/api/API_CONTRACTS.md`, `docs/ai/ARCHITECTURE_DECISIONS.md`,
`bricks/runtime/{transactions.conf,web_routes.conf}`,
`bricks/runtime/rexx/{auth,cdrin}.rexx`.

## What 2.5 is

A cleanup release, not a feature release (`CARDMAN_2.5_PLAN.md` section 1, 6):
native BRICKS verbs, shell-to-REXX gates, actor-stamping standardization,
docs consolidation. It explicitly makes **no money-behaviour change and no
WAPI JSON contract change**; existing routes stay byte-identical. So 2.5
itself does not break this integration. The problems below come from
contract details that existed before 2.5 and that the integration repo's
bridge and our earlier docs did not match.

## Findings

| # | Sev | Finding | CARDMAN evidence | Effect on this fork |
|---|---|---|---|---|
| C1 | B | **CAUT requires `seq_num` and `requested_at`.** Missing either returns 400 `BAD_REQUEST`. The `auth_code` is a pure function of `(pan, seq_num, requested_at)`, which is how CAUT dedupes retries. | `auth.rexx` "seq_num and requested_at are required", `DERIVE_AC`; `API_CONTRACTS.md` CAUT request | The integration bridge sends neither, so every real authorization would fail and (correctly) decline. mm_manager has `auth_request->seq` but does not forward it. |
| C2 | H | **CDRI rates the call itself (ADR 8).** Inbound `charge_cents` is required (non-negative integer) but only stored as `reported_charge_cents`; the billed amount is computed by BRICKS' tariff engine from duration and `start_ts`. | `cdrin.rexx` header and validation; ADR 8 | The earlier plan (bridge computes `charge_cents` from `GET /api/rates`) is obsolete. The bridge's `RateCache` can go. Send the terminal's own `call_cost` as the reported value, or 0. |
| C3 | H | **`international` drives call type.** CAUT sizes the hold by call type: toll-free from the NPA, else `international: yes` gives `INTL`, else `DOM`. | `auth.rexx` call_type derivation | The bridge sends `"unknown"`, so international calls are held at domestic rates. The FUNF auth request carries `call_type` (CALLTYP; low nibble `CALL_TYPE_INTERNATIONAL` = 0x06, `mm_manager.h` ~L437), so mm_manager can derive it, but does not forward it. |
| C4 | M | **WAPI identity.** CAUT ACL: `platform,agi,admin,gate`. CDRI ACL: `mediation,agi,admin`. `GET /api/rates`: `mediation,admin`. There is no Millennium-specific group. `millennium` is already the **database binding name** (5th field of every money transid in `transactions.conf`). | `web_routes.conf` L7, L15, L73; `transactions.conf` L67, L72 | Naming a new WAPI group `millennium` (integration repo suggestion) would be confusing. Use a distinct name (e.g. `payphone`). Until CARDMAN adds one, `agi` is the only group granting both CAUT and CDRI. |
| C5 | M | **CAUT dispositions.** `approved`, `approved_limited` (smaller hold, fraud cap), `requires_card`, `declined`, `bad_card`, `bad_country`, `no_tariff`. | `API_CONTRACTS.md` CAUT rules | The wire response is only valid/invalid, so `approved_limited` cannot be signalled to the terminal. The call proceeds against a smaller hold; any overage becomes a CDRI `SHORTFALL` in AUDIT. Same root cause as G6 (no mid-call cutoff). |
| C6 | M | **CDRI parks failures server-side.** `UNMATCHED` and `NOTARIFF` are durable states retried by the `CDRS` sweep; `DUPLICATE` echoes the prior state; `FILTERED` drops calls under `min_cdr_duration_secs` (default 3 s). | `API_CONTRACTS.md` CDRI states | Once CDRI returns any well-formed response, CARDMAN owns the record. Only transport errors and `INTERNAL` need local retry. This narrows plan decision D1. |
| C7 | M | **`answer_ts` matters** for ring-no-answer vs connected and for multi-leg merge priority. Optional. | `API_CONTRACTS.md` CDRI | mm_manager has no answer timestamp, only `call_type`. Omit `answer_ts`; skip submission for `CALL_TYPE_UNANSWERED` as `CDR_CONTRACT_SCOPING.md` already proposed. |
| C8 | Low | **PAN must be 16 digits** (or a `dial_id`). Card number is the TN (ADR 10). | `cdrin.rexx` validation; ADR 10 | mm_manager's PAN decode must yield exactly 16 digits for CARDMAN cards; validate before sending (fits task T5). |
| C9 | Info | **PIN verification works** in the database (`crypt()` bcrypt compare, `auth.rexx` ~L422). `auth.rexx`'s header comment saying PINVERIFY "cannot run here yet" is stale. 4-digit PINs match CARDMAN's model. | `auth.rexx` L12-17 vs L422 | No change. Stale comment is CARDMAN's to fix. |
| C10 | Info | **BRICKS is unpinned (ADR 39)**, currently 3.4.x; CARDMAN policy keys moved to `cardman.cnf`. Actor identity is resolved from the mTLS CN via one helper (ADR 31). WAPI service identities are telco-global (ADR 38). | ADR 31, 38, 39 | One bridge identity is the actor for every payphone. Terminal identity must therefore travel in CAUT's `terminal` object (`terminal_id`, `phone_number`, `terminal_serial`, `telco_id`) to keep "no anonymous state changes" meaningful. |
| C11 | M | **`cdr_id` must be unique forever.** CDRI dedupes on `cdr_id` and answers a repeat with `DUPLICATE` (no billing). | `API_CONTRACTS.md` CDRI | The fork's `mm-<terminal_id>-<seq>` uses a 16-bit `seq`, which wraps (or resets on terminal re-init). A wrapped id would silently skip billing. Add a disambiguator such as `start_ts` (e.g. `mm-<terminal>-<seq>-<start_ts>`), still stable across resends of the same record. |

## Required changes (summary)

Tracked in the plan as phase **P1b** (`COMPATIBILITY_PLAN.md`) and
tasks T13 (fork) and T16 (bridge).

In **this fork** (new task T13):
- Forward `seq` (as `seq_num`), a stable `requested_at`, `international`
  derived from `call_type`, and the terminal's phone number to the bridge on `/authorize`.
- Forward the terminal's reported `call_cost` and `call_type` on `/cdr`;
  skip unanswered calls; make `cdr_id` wrap-safe (C11).
- `requested_at` must be identical for retries of the same
  `(terminal, seq)` within a session, or CAUT will place a second hold.

In the **integration repo's bridge** (owner action; not edited from here):
- Pass `seq_num`, `requested_at`, `international`, and a full `terminal`
  object to CAUT.
- Drop `RateCache`; send the reported charge.
- Treat CDRI `INTERNAL` and transport errors as retryable; every other
  state as delivered.
- Choose a WAPI group name other than `millennium`.

On the **CARDMAN side** (owner/CARDMAN review):
- New WAPI group for this front end (or accept reuse of `agi`).
- Optionally fix the stale PIN comment in `auth.rexx`.

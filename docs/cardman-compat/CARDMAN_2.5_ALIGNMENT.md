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
| C4 | M | **WAPI identity. Resolved in 2.5.0.1.** New group `payphone`, granted CAUT and CDRI only; cert CN = Basic-Auth userid = `mm-<env>` (dev `mm-dev`), one per bridge deployment. Every other route is 403 to it, including `GET /api/rates`. | ADR 40; `MILLENNIUM_GATEWAY_CONTRACT.md` §2-3; `web_routes.conf` L9, L17 | Bridge uses the `payphone` identity (`tools/cardman_bridge.py`). Actor stamps: `platform:mm-<env>` on HOLD, `mediation:mm-<env>` on CAPTURE. |
| C5 | M | **CAUT dispositions. Decided:** only `approved` is valid at the terminal. `approved_limited` (150 c fraud cap) is **invalid**, and the bridge immediately sends a zero-second release CDR so the hold is `RELEASED`. `declined`, `bad_card`, `requires_card`, `bad_country`, `no_tariff`, non-200, timeout, unparseable: invalid. | ADR 40 decision 5; contract §4 | Implemented in the reference bridge (`terminal_verdict()`, `send_release()`). |
| C6 | M | **CDRI parks failures server-side.** `UNMATCHED` and `NOTARIFF` are durable states retried by the `CDRS` sweep; `DUPLICATE` echoes the prior state; `FILTERED` drops calls under `min_cdr_duration_secs` (default 3 s). | `API_CONTRACTS.md` CDRI states | Once CDRI returns any well-formed response, CARDMAN owns the record. Only transport errors and `INTERNAL` need local retry. This narrows plan decision D1. |
| C7 | M | **Corrected: send a CDR for every approved authorization, including unanswered calls.** The earlier "skip unanswered calls" was wrong. `payphone` has no void route and CARDMAN never expires an `OPEN` hold; a CDR with the `auth_code` and `duration_secs` < 3 is the only release (`FILTERED`). | ADR 40 decision 7; contract §5 | Fork forwards every CDR that carries a CARDMAN `auth_code`; the bridge sends `duration_secs` 0 for unanswered/abandoned/denied call types so ring time is never billed. `answer_ts` is omitted (mm_manager has none). |
| C8 | Low | **PAN must be 16 digits** (or a `dial_id`). Card number is the TN (ADR 10). | `cdrin.rexx` validation; ADR 10 | mm_manager's PAN decode must yield exactly 16 digits for CARDMAN cards; validate before sending (fits task T5). |
| C9 | Info | **PIN verification works** in the database (`crypt()` bcrypt compare, `auth.rexx` ~L422). `auth.rexx`'s header comment saying PINVERIFY "cannot run here yet" is stale. 4-digit PINs match CARDMAN's model. | `auth.rexx` L12-17 vs L422 | No change. Stale comment is CARDMAN's to fix. |
| C10 | Info | **BRICKS is unpinned (ADR 39)**, currently 3.4.x; CARDMAN policy keys moved to `cardman.cnf`. Actor identity is resolved from the mTLS CN via one helper (ADR 31). WAPI service identities are telco-global (ADR 38). | ADR 31, 38, 39 | One bridge identity is the actor for every payphone. Terminal identity must therefore travel in CAUT's `terminal` object (`terminal_id`, `phone_number`, `terminal_serial`, `telco_id`) to keep "no anonymous state changes" meaningful. |
| C11 | M | **`cdr_id` must be unique forever. Accepted:** `mm-<terminal>-<seq>-<start_ts>`, byte-identical on every resend (terminal start time, never receive time), no `"` or `\`. | Contract §5 `cdr_id`; live-proven in `verify-payphone.sh` | Implemented in `cardman_forward_cdr()`. |
| C12 | H | **7-digit numbers must be expanded with the payphone's NPA.** CDRI has no international flag and rates any number that is not 10 or 1+10 digits as `INTL`. | Contract §4-5; `cdrin.rexx` `DERIVE_CALLTYPE` | Bridge `normalize_number()`: 7 -> NPA+7, 0/1+7 -> NPA+7, 0+10 -> 10. |
| C13 | B | **`auth_code` is 8 chars of `[0-9A-Z]`, not hex.** The fork hex-decoded it into 32 bits, so nearly every real code would have been mangled and its CDR would park as `UNMATCHED`. | `auth.rexx` `DERIVE_AC` | Fork carries the base-36 value (< 36^8, 42 bits) in the 64-bit wire field. Terminal echo of all 64 bits is unverified on hardware. Request R3. |
| C14 | M | **CDRI cannot be told a call is international** (R4); a 10-digit international number is rated `DOM`. | `cdrin.rexx` | Recorded as request R4; no bridge workaround. |
| C15 | Info | **The `terminal` object is accepted but not stored** in 2.5.0.1. | Contract §4 | Sent anyway; attribution comes from CDR `switch_id` and `ani`. |

Updated 2026-10-13 against MCardSys `v2.5.0.1` (`90bcb47`). Full hop
contract and the code audit: [`BRIDGE_CONTRACT.md`](BRIDGE_CONTRACT.md).
Open asks for CARDMAN: [`CARDMAN_REQUESTS.md`](CARDMAN_REQUESTS.md).

## Required changes (summary)

Tracked in the plan as phase **P1b** (`COMPATIBILITY_PLAN.md`) and
tasks T13 (fork) and T16 (bridge).

In **this fork** (new task T13):
- Forward `seq` (as `seq_num`), a stable `requested_at`, `international`
  derived from `call_type`, and the terminal's phone number to the bridge on `/authorize`.
- Forward the terminal's reported `call_cost` and `call_type` on `/cdr`;
  **do not skip unanswered calls** (C7); make `cdr_id` wrap-safe (C11).
  Done in this branch (T13).
- `requested_at` must be identical for retries of the same
  `(terminal, seq)` within a session, or CAUT will place a second hold.

In the **bridge**: implemented as the reference bridge
`tools/cardman_bridge.py` in this repo; porting it to the integration repo
is task T16 (`AI/TASKS.md`). It uses group `payphone` (C4), maps
dispositions per C5, sends release CDRs (C5, C7), expands 7-digit numbers
(C12), drops `RateCache`, sets `source: mm_manager`, fails closed, and
retries with identical bodies.

On the **CARDMAN side**: pre-work done in 2.5.0.1 (see
[`PREWORK.md`](PREWORK.md)); remaining asks in
[`CARDMAN_REQUESTS.md`](CARDMAN_REQUESTS.md).

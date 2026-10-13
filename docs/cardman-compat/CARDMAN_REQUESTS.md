# Requests for the CARDMAN owner

From the mm_manager side of the Millennium integration, after reading
MCardSys `v2.5.0.1` (`90bcb47`). Nothing here was changed in MCardSys.
Each item says what we need, why, and the evidence.

| # | Request | Why | Evidence |
|---|---|---|---|
| R1 | **A way to release an orphaned payphone hold**: a hold expiry/sweep for `OPEN` holds older than N hours, or a narrow void grant for `payphone`. | The release CDR is the payphone's only void path. A hold leaks for good if the approval is lost between bridge and mm_manager after the bridge wrote it, if the terminal never produces a CDR for an approved call that does not connect (unverified on hardware), or if a CDR is lost before G1/T7 lands. CARDMAN "never expires an open hold". | Contract §5; ADR 40 decision 7 |
| R2 | **State which CAUT `variant` the payphone must use.** The bridge sends `funf` because the terminal collects a PIN; a card with `pin_state` `UNSET` then answers `requires_card`. | The contract and `verify-payphone.sh` (which uses `simple`) do not say. `simple` would skip PIN verification. | `auth.rexx` `IF VARIANT = 'funf'`. Live (2026-10-13): `funf` with a NEWC-issued card's PIN is `approved`, with a wrong PIN `declined`, so `funf` works for NEWC cards; the request is for the contract to say so. |
| R3 | **Document the `auth_code` alphabet** (8 chars of `[0-9A-Z]`, base-36) in `API_CONTRACTS.md`. Its examples (`A1B2C3D4`) read as hex; our fork decoded it as hex until now. | Front ends carrying the code in a numeric field need its range (below 36^8). | `auth.rexx` `DERIVE_AC` |
| R4 | **An explicit call type or international flag on CDRI.** CDRI rates any number not 10 or 1+10 digits as `INTL`, and a 10-digit international number (e.g. `011` + 7 digits) as `DOM`. CAUT has the flag; CDRI does not, so hold and charge can disagree. | Correct rating of short international numbers. | `cdrin.rexx` `DERIVE_CALLTYPE` |
| R5 | **Per-source hold policy for `payphone`** (already proposed for 2.6). | No mid-call cutoff: overage is an uncollected `CDR_SHORTFALL` even on a funded card. | Contract §4; `CARDMAN_2.5.1_TODO.md` |
| R6 | **Quote route `POST /api/rates/quote` with `payphone` read access** (already proposed for 2.6). | Terminal rate tables that match billing (O1) without a `mediation` credential on the payphone host. | Contract §6 |
| R7 | **Record CAUT's `terminal` object** (already deferred). Until then only the CDR (`switch_id`, `ani`) attributes a call to a terminal; a declined or `approved_limited` auth has no terminal attribution in CARDMAN. | Fleet fraud review per terminal. | Contract §4, §8 |
| R8 | **Confirm `start_ts` handling for the release CDR.** The bridge sends `start_ts` = the auth's `requested_at` and `cdr_id` `mm-<terminal>-A<seq>-<requested_at>` for an `approved_limited` release. Say if CARDMAN wants a different id shape for bridge-originated records. | Avoids a future id clash with terminal CDRs. | Contract §5 `cdr_id`. Live (2026-10-13): accepted, `FILTERED`, the 150c hold `RELEASED`. |

Live verification of these flows on the mm_manager side is
`tools/cardman_live_test.sh`. Its first run against CARDMAN 2.5.0.1
(2026-10-13) found no CARDMAN behaviour that differs from its contract,
so it added no request here.

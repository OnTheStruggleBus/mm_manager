# CARDMAN compatibility plan

Planning documents for turning this fork's CARDMAN prototype into a
version that can safely front real CARDMAN card traffic. Docs only; the
code they describe is in `src/mm_cardman*.c` and `tools/cardman_*.py`.

| File | What it covers |
|---|---|
| [`ARCHITECTURE_ANALYSIS.md`](ARCHITECTURE_ANALYSIS.md) | Current-state architecture of the fork, the auth and CDR data flows, and the gaps found, with file:line citations. |
| [`COMPATIBILITY_PLAN.md`](COMPATIBILITY_PLAN.md) | Target architecture, phased roadmap, acceptance criteria, risks, and decisions needed from the owner. |
| [`THREAT_MODEL.md`](THREAT_MODEL.md) | Every place card data (PAN/PIN) flows or lands, and the control each one needs. |
| [`PREWORK.md`](PREWORK.md) | CARDMAN-side pre-work (done in MCardSys before live testing), with a ready-to-use prompt for a CARDMAN session. |
| [`CARDMAN_2.5_ALIGNMENT.md`](CARDMAN_2.5_ALIGNMENT.md) | Contract check against CARDMAN (MCardSys) 2.5 / 2.5.0.1: what changed, what the bridge and fork must send. |
| [`BRIDGE_CONTRACT.md`](BRIDGE_CONTRACT.md) | The mm_manager <-> bridge <-> CARDMAN hop contract, and the code audit it came from. |
| [`CARDMAN_REQUESTS.md`](CARDMAN_REQUESTS.md) | Open requests for the CARDMAN owner. |
| [`HARDWARE_TEST_PLAN.md`](HARDWARE_TEST_PLAN.md) | Checklist for a real terminal: what only hardware can prove, the SQL to check each step, and a results table. |
| [`HARDWARE_TEST_SESSION.md`](HARDWARE_TEST_SESSION.md) | Prompt for a test-day session that walks you through the plan step by step. |

The production bridge, the terminal configuration API (adverts, rate
tables) and the shared vocabulary are specified separately in
[`../wapi-bridge/`](../wapi-bridge/README.md) (ADR-0001, proposed).

Agent-facing task cards derived from this plan live in
[`../../AI/TASKS.md`](../../AI/TASKS.md).

## Relationship to the integration repo

Earlier research and decisions live in
[`OnTheStruggleBus/-mm-cardman-integration`](https://github.com/OnTheStruggleBus/-mm-cardman-integration)
(`INTEGRATION_PROPOSAL.md`, `SOW.md`, `CDR_CONTRACT_SCOPING.md`,
`bridge/`). This plan builds on those decisions rather than reopening
them: minimal fork + separate bridge process holding the WAPI identity
(ADR 11 / Tranz pattern), CAUT for authorization, CDRI for CDRs.

One earlier decision is superseded: the integration repo planned for the
bridge to compute `charge_cents` from `GET /api/rates`. Under CARDMAN
ADR 8, CDRI rates every call itself and does not trust an inbound
`charge_cents`. See [`CARDMAN_2.5_ALIGNMENT.md`](CARDMAN_2.5_ALIGNMENT.md).

What this plan adds is an audit of the fork **as it exists on `main`
today** (after PRs #1 to #6) and the remaining work to call it
CARDMAN-compatible.

## Status as of 2026-10-13

- Fork code reviewed at commit `3e173a3`.
- Integration repo docs read at commit `687898c` (read-only).
- CARDMAN (`MCardSys`) 2.5 checked at commit `4e9a6eb`; 2.5.0.1
  (`90bcb47`, tag `v2.5.0.1`) re-checked the same day (read-only).
- Validated against a live CARDMAN 2.5.0.1 dev container on 2026-10-13:
  `tools/cardman_live_test.sh` passes (P1b; see `COMPATIBILITY_PLAN.md`
  and `AI/TASKS.md` T13). Nothing has been validated on a physical
  terminal yet; the open hardware checks are in `BRIDGE_CONTRACT.md`
  section 4.

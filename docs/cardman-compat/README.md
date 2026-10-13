# CARDMAN compatibility plan

Planning documents for turning this fork's CARDMAN prototype into a
version that can safely front real CARDMAN card traffic. **Docs only.**
No code in this directory, and nothing here changes runtime behavior.

| File | What it covers |
|---|---|
| [`ARCHITECTURE_ANALYSIS.md`](ARCHITECTURE_ANALYSIS.md) | Current-state architecture of the fork, the auth and CDR data flows, and the gaps found, with file:line citations. |
| [`COMPATIBILITY_PLAN.md`](COMPATIBILITY_PLAN.md) | Target architecture, phased roadmap, acceptance criteria, risks, and decisions needed from the owner. |
| [`THREAT_MODEL.md`](THREAT_MODEL.md) | Every place card data (PAN/PIN) flows or lands, and the control each one needs. |

Agent-facing task cards derived from this plan live in
[`../../AI/TASKS.md`](../../AI/TASKS.md).

## Relationship to the integration repo

Earlier research and decisions live in
[`OnTheStruggleBus/-mm-cardman-integration`](https://github.com/OnTheStruggleBus/-mm-cardman-integration)
(`INTEGRATION_PROPOSAL.md`, `SOW.md`, `CDR_CONTRACT_SCOPING.md`,
`bridge/`). This plan builds on those decisions rather than reopening
them: minimal fork + separate bridge process holding the WAPI identity
(ADR 11 / Tranz pattern), CAUT for authorization, CDRI for CDRs, bridge
computes `charge_cents` from `GET /api/rates`.

What this plan adds is an audit of the fork **as it exists on `main`
today** (after PRs #1 to #6) and the remaining work to call it
CARDMAN-compatible.

## Status as of 2026-10-13

- Fork code reviewed at commit `3e173a3`.
- Integration repo docs read at commit `687898c` (read-only).
- Nothing in this plan has been validated against a live CARDMAN
  instance or physical terminal; items that depend on that are marked.

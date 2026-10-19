# ADR-0001: WAPI bridge, terminal configuration API, and a shared vocabulary

- **Status**: Proposed, 2026-10-18. Owner decisions DW-1 to DW-6 are open.
  Independently reviewed 2026-10-18 (`ADVERSARIAL_REVIEW.md`, second
  section); its fixes are in.
- **Scope**: this fork (`OnTheStruggleBus/mm_manager`) and the processes
  that run beside it on the payphone host. CARDMAN changes appear only as
  requests (R9-R11).
- **Detail**: [`SPEC.md`](SPEC.md) (architecture),
  [`VOCABULARY.md`](VOCABULARY.md) (field dictionary, events, enums),
  [`ADVERSARIAL_REVIEW.md`](ADVERSARIAL_REVIEW.md) (red-team findings and
  what changed because of them).

## Context

- The card path is designed and live-proven against CARDMAN 2.5.0.1:
  mm_manager -> loopback -> bridge -> `CAUT`/`CDRI`
  (`docs/cardman-compat/BRIDGE_CONTRACT.md`, reference bridge
  `tools/cardman_bridge.py`, port checklist `AI/TASKS.md` T16). The
  settled decisions in `AI/CARDMAN_CONTEXT.md` stand.
- The `payphone` WAPI identity may call `CAUT` and `CDRI` only (CARDMAN
  ADR 40). `GET /api/rates` needs `mediation` or `admin`; a quote route
  (`QUOT`) is proposed for CARDMAN 2.6, not built.
- Operators also need to change what terminals show and charge locally:
  advertising prompts (DLOG `0x1d`) and rate tables (`0x49`, `0x97`,
  NPA-NXX/LCD). Today those are hand-built files under `tables/`. The
  CARDMAN gateway contract §1 assigns rate-table encoders and table
  download to this fork; CARDMAN has no advert concept at all.
- A routine scheduled call-in does not download tables
  (`case DLOG_MT_CALL_IN`: the download lines are commented out); only a
  terminal-initiated table update does.
- The RATE table is terminal-wide: it prices coin calls as well as the
  cost the terminal reports for card calls.
- Several systems describe the same call and card facts with different
  names (`call_type` means a CALLTYP integer on the bridge hop and
  `DOM|INTL|TOLLFREE` in CARDMAN), and the owner wants one vocabulary
  that other apps can integrate with.

## Decisions

**W1. Two planes, two processes.** The card plane (`mm-bridge`) carries
PAN/PIN and talks only to mm_manager (loopback/Unix socket) and CARDMAN
TERMREG. The config plane (`mm-termcfg`) owns adverts, rate plans,
deployments, audit and event relay, and never sees card data. They
share no listener, no credential, and no process. The bridge also
refuses unenrolled terminals and applies per-card, per-terminal and
fleet attempt limits, because `payphone` has no CARDMAN rate limit and a
DLOG emulator could otherwise lock customers' cards with wrong PINs
(review A-1). The per-card limit is what stops lockouts; the fleet limit
only alerts and tightens, because a fleet-wide refusal would let anyone
stop every card call (review I-6). `mm-termcfg` writes only the advert
and rate tables; it cannot write the card table or any other table that
controls the card path (review I-3).
*Why*: the config
plane needs operator and app access; putting that on the process holding
PANs would widen the card-data attack surface that `SECURITY_RULES.md`
rules 2, 4 and 5 keep narrow, and a config bug must never be able to
decline cards or leak a PAN.

**W2. Systems of record.** CARDMAN owns money, tariffs, holds and
rating (ADR 8). `mm-termcfg` owns terminal display configuration
(adverts, the terminal price list as published, deployments). The
terminal price list may be *derived* from CARDMAN's tariff, but the
published plan is a separate, approved artifact.

**W3. Vocabulary: adopt CARDMAN's names, CloudEvents for events.** PIV
v1 ([`VOCABULARY.md`](VOCABULARY.md)) uses CARDMAN's field names as
canonical, disambiguates collisions with qualified names
(`dlog_call_type`), puts units in names, classifies every field S0-S3,
and wraps integration events in CloudEvents 1.0. It maps the live wire
contracts; it does not rename them. *Rejected*: renaming the proven
bridge-hop fields to canonical names (risk on the money path for no
gain); inventing a new naming scheme (a second contract that drifts from
CARDMAN, whose docs win).

**W4. Advertising prompts get a validated, versioned, approved API.**
An `advert_set` is all 20 slots; rejected, never truncated, on any
violation; compiled with the existing encoder logic; previewed by
round-trip decode; approved (two-person by default) before deployment.
Digit runs and prices in advert text need an explicit approval reason.
*Why*: the payphone display is a surface callers trust; a hijacked
advert is a fraud vector.

**W5. Rate tables are compiled from an approved `rate_plan`; CARDMAN is
an input, never an auto-publisher.** Sources: manual, an operator-run
export of `GET /api/rates` uploaded as a file (usable now), a scheduled
pull with a dedicated read-only `ratesync` identity (after CARDMAN
request R9), and `QUOT` sampling (CARDMAN 2.6). The `mediation`
credential is never placed on the payphone host. Every import yields a
draft and a diff, labelled `exact` or `approximate`, with lossy entries
flagged. *Why*: `mediation` also grants `CDRI`, a money-moving route;
the `/api/rates` response omits bands, periods, fees and overrides; and
the RATE table also prices coin calls (DW-1).

**W6. Staging is atomic per set, delivery is proven.** Tables are
written tmp + fsync + rename into the store mm_manager already reads,
under a lock mm_manager also holds for a whole download, so a terminal
never receives half old and half new tables (review A-10); the previous
file is kept. A table is `delivered` only when a new fork hook
records a receipt whose hash matches, at the point where
`wait_for_table_ack()` returns success during the download (review I-1).
Because a cut-off download can still leave a terminal with new RATE and
old prefix tables, a plan version may not change what an index means
while the published prefix map points at it (review I-2). Delivery
on routine call-in (F2) is built behind a flag and enabled only after
hardware check H-W1.

**W7. Config plane identity is per client, from its own CA.** mTLS with
a config-plane CA separate from CARDMAN's, one identity per person or
app, roles, rate limits, append-only audit. Unix socket by default; the
HTTPS listener is opt-in.

**W8. Integration is by masked, signed events.** The bridge writes
masked events to a local spool; `mm-termcfg` relays them as
signed CloudEvents to HTTPS subscribers (no internal targets). The relay
drops any event carrying an S3 field or a Luhn-valid digit run. The bridge keeps CARDMAN as its only egress.
Events carry no card identifier by default. `events.sensitive`
subscribers get `masked_pan` and a keyed hash (`card_ref`) re-keyed per
subscription; both are S2, because a stable per-card id is a cardholder
trail whether or not it is hashed (review I-4). `dial_id` is S3: under
CARDMAN ADR 10 it nearly determines the PAN (review I-5).

**W9. mm_manager answers `DLOG_MT_RATE_REQUEST` from the plan the
terminal actually holds** (a compiled local file per published version,
chosen by the terminal's last RATE receipt, not the newest plan; review
I-7), replacing the hard-coded answer, with no network call inside the
protocol loop.

**W10. No change to CARDMAN money behaviour, the DLOG protocol, or the
bridge hop's wire format.** The bridge hop gains only an optional
`piv_version` marker and a `GET /healthz`.

## Owner decisions needed

| # | Decision | Options | Recommendation |
|---|---|---|---|
| DW-1 | Coin vs card pricing share one RATE table | (a) one price list for both, CARDMAN tariff set to match coin prices; (b) terminal table stays coin-authored, CARDMAN import used only to report drift; (c) find a terminal mechanism to separate them (none known) | **(b) now**, move to (a) if the owner wants card and coin prices identical. Confirm with H-W3 first. |
| DW-2 | How `mm-termcfg` gets CARDMAN tariff data | `operator_file`; `ratesync` identity (R9); wait for `QUOT`; `mediation` | **`operator_file` now, R9 next, `QUOT` when built. Never `mediation`.** |
| DW-3 | Approval rule | two-person; single-operator with recorded self-approval | Two-person where there are two operators; single-operator mode otherwise, always marked in audit and events. |
| DW-4 | Advert content policy | none; flag digit runs and prices; forbid them | **Flag** (approval reason required). |
| DW-5 | Enable F2 (download on routine call-in) | on, off | Off until H-W1 passes. |
| DW-6 | `mm-termcfg` implementation language | Python stdlib; Go | **Python stdlib** (reuses `generate_*.py` encoders; matches the reference bridge). |

## Consequences

- Two new processes to supervise on the payphone host (bridge, termcfg)
  in addition to one mm_manager per line.
- Five small fork changes (F1-F5 in `SPEC.md` §3.1; F4 is documentation only), each in a
  new `mm_termcfg*.c` file, so upstream merges stay cheap.
- Until H-W1 passes, publishing a change still needs a technician-forced
  download; the API says `staged`, not `delivered`.
- Displayed prices stay labelled approximate unless the CARDMAN tariff is
  all-wildcard with no fees, or until `QUOT` exists.
- Other apps can integrate through events and the config API without
  any access to card data or to CARDMAN credentials.

## Links

- Fork: `docs/cardman-compat/{BRIDGE_CONTRACT,COMPATIBILITY_PLAN,THREAT_MODEL,CARDMAN_REQUESTS}.md`,
  `AI/SECURITY_RULES.md`, `AI/TASKS.md` T14-T16.
- CARDMAN: `docs/frontends/MILLENNIUM_GATEWAY_CONTRACT.md`, ADR 8, 11,
  13, 15, 18, 26, 38, 40, 42; `docs/api/API_CONTRACTS.md`.

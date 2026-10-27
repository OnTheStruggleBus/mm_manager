# CARDMAN compatibility plan

Goal: a build of this fork that can front real CARDMAN card traffic
without losing billable calls or exposing card data, while staying a
small, rebasable delta on upstream `hharte/mm_manager`.

Gap IDs (G1 to G13) refer to [`ARCHITECTURE_ANALYSIS.md`](ARCHITECTURE_ANALYSIS.md).

## 0. Pre-work (CARDMAN side)

CARDMAN changes this front end depends on (WAPI group, rate data,
`/api/rates` fixes and docs, contract confirmations, ADR) are tracked
separately in [`PREWORK.md`](PREWORK.md) and done in MCardSys, outside
this roadmap. Phases below name the pre-work item they wait on.

## 1. Target architecture

```
Nortel Millennium terminal
   |  1200 baud Bell 212A dial-in
   v
[ hardware modem ]  or  [ ATA -> Asterisk -> datamodem -> mm-softmodem-dte PTY ]   (docs/softmodem, ADR-0002)
   |  serial / PTY  (-f <dev> -m)
   v
mm_manager  (CARDMAN build: MM_CARDMAN=ON)
   - DLOG protocol, local config tables, local SQLite (masked only)
   - CARDMAN adapter: auth, CDR (ACK held on failure), redaction  <- fork delta
   |  loopback only (127.0.0.1 or Unix socket), HTTP/JSON
   v
cardman_bridge.py  (tools/ here; port to integration repo = T16; payphone identity, mTLS)
   |  CAUT / CDRI only (GET /api/rates is 403 to payphone)
   v
CARDMAN (BRICKS)
```

Design rules:

1. **Decisions about money live in CARDMAN.** mm_manager never approves
   locally, and neither mm_manager nor the bridge computes the settled
   charge: CDRI rates every call (CARDMAN ADR 8).
2. **Card data has exactly one clear-text path:** terminal -> mm_manager
   memory -> loopback -> bridge. Every other sink is masked or removed.
3. **Every CDR the terminal hands over is either accepted by CARDMAN or
   held durably for retry.** No path loses a call silently.
4. **Fork delta stays isolated:** new files (`mm_cardman_*.c`), the two
   `case` blocks, accounting sinks, and diagnostic redaction hooks.
   Everything compiles out with `MM_CARDMAN=OFF` so upstream behavior is
   reproducible.

### Components to add (design only)

| Component | Purpose | Gaps |
|---|---|---|
| `MM_CARDMAN` CMake option | Builds the bridge client, enables redaction, disables raw-byte diagnostics. Default ON in this fork's release workflow, OFF reproduces upstream. | G2, G9 |
| CDR retention (D1) | Preferred: withhold the CDR ACK so the terminal keeps the record. A local outbox would store an **unmasked PAN**, which `AI/SECURITY_RULES.md` rule 2 forbids; it is **not** a supported fallback unless the owner explicitly amends that rule (with encryption at rest, a retention cap, and an outage alarm). | G1 |
| Diagnostic redactor | One function that, given a decoded packet, zeroes card-number and PIN bytes of table 0x01/0x02 before `print_mm_packet`, pcap, UDP, and UART log writes. | G2 |
| Bridge address guard | Reject non-loopback `CARDMAN_BRIDGE_ADDR` unless an explicit override is set; optional Unix-domain socket. Validate PAN/PIN/called number are digits before building JSON. | G3 |
| Unknown-auth guard | Explicit `case` for 0x01/0x04/0x3d/0x46: parse by struct size where known, decline any auth request, log and ACK-hold CDR-like records. | G4 |
| CARDMAN card-table profile | A documented `tables/cardman/` card table with CARDMAN BIN ranges, `CARD_VF_NCCVAL_IND` set (manager validation required). | G5 |
| Rate-table generator | Script (sibling to `generate_lcd.py`) that emits terminal rate tables from CARDMAN tariff data, so displayed prices approximate what CDRI will charge. Not buildable on the `payphone` credential; see O1 for the access options. | G8 |
| CAUT/CDRI payload completion | Forward `seq_num`, stable `requested_at`, `international` (from `call_type`), terminal phone number; on CDRs forward `call_cost`, `call_type` and line number, and **send unanswered calls too** (their CDR releases the hold). Done: `src/mm_cardman.c`, `tools/cardman_bridge.py`. | G14 |
| Replay test harness | `.dlog` fixtures with synthetic test PANs, run in `ctest` against `tools/cardman_stub_bridge.py`. | G10 |

## 1a. Future-state objectives

### O1. Terminal rate tables loaded from CARDMAN

Today terminal rate tables are hand-built files in `tables/`; CARDMAN
rates the real charge (ADR 8). Target: CARDMAN's tariff master is the
single source, and the prices a caller sees are generated from it.

- **Access (decided in CARDMAN 2.5.0.1, ADR 40)**: the `payphone`
  identity **cannot** read `GET /api/rates` (403). Do not build O1 on the
  payphone credential. Options, owner's choice:
  1. a separate sync job with its own `mediation` credential. That group
     also grants CDRI, so it is a wider grant than the job needs;
  2. hand-maintained tables (today's state);
  3. wait for CARDMAN 2.6's proposed `POST /api/rates/quote` (`QUOT`,
     ACL `payphone,mediation,admin`, read-only), which returns CDRI's
     own computed charge and is exact by construction (request R6).
- **Source** (option 1): `GET /api/rates` (`ratesrv.rexx`, ACL
  `mediation,admin`, 30 req / 60 s per actor). Rows of `telco_id`,
  `call_type`, `carrier`, `initial/second/addl` cents and secs,
  `version`, `effective_ts`, plus `count`, ordered by
  `telco_id, call_type, carrier`.
- **Pipeline**: the sync job pulls on a schedule -> validates -> maps
  rows to terminal `DLOG_MT_RATE_TABLE` / `DLOG_MT_INTL_SBR_TABLE` / LCD
  tables with the existing `generate_*.py` encoders -> writes them to the
  per-terminal table directory -> mm_manager's existing
  `check_mm_table_is_newer()` download path sends them on the terminal's
  next call-in. No change to the DLOG protocol.
- **Change detection**: the earlier `(telco_id, call_type, carrier) ->
  version` map is **wrong**: rows that differ only by distance band or
  time-of-day period come back with the same key, so one silently
  overwrites the other. Compare the whole list: `count` plus every row's
  `(telco_id, call_type, carrier, version, effective_ts)` in order.
  `effective_ts` has minute precision on the 2.5 line
  (`2026-10-13T14:02Z`, no seconds): never parse it as full ISO-8601
  seconds or use it alone to order edits; `version` does that.
- **Fail-safe**: anything but `"ok":true` keeps the last good tables. As
  of 2.5.0.1 a mid-list fetch error returns `INTERNAL` instead of a
  partial list. Also reject zero rows, a `count` that differs from the
  list length, and parse errors. Never push an empty or partial table.
- **Tolerance**: base rows equal CDRI's bill only when the tariff uses
  no per-number overrides, no non-`*` distance band or period rows, no
  connection fee, minimum charge or surcharge, and no carrier-specific
  rows. The response cannot show which case applies (those columns are
  not in it). So the displayed price is advisory: state it as "exact for
  an all-wildcard tariff, otherwise approximate", and measure the gap
  with the acceptance matrix below. Option 3 removes the gap.
- **Acceptance**: changing a rate in CARDMAN (`RADM`) reaches a test
  terminal's display on its next call-in; a fixed call matrix shows
  terminal price equal to CDRI `charged_cents` for an all-wildcard
  tariff, and the measured difference documented otherwise.

### O2. A solid CARDMAN API integration

Make the mm_manager -> bridge -> CARDMAN chain correct by contract, not
by coincidence.

1. **One written contract per hop.** mm_manager <-> bridge contract
   versioned in this repo (JSON schema for `/authorize`, `/cdr`, rate
   sync output). Bridge <-> CARDMAN pinned to MCardSys
   `docs/api/API_CONTRACTS.md` and `ratesrv.rexx` by commit.
2. **Strict validation both ways.** Requests: digits-only PAN (16),
   PIN (4), called number; `seq_num`, `requested_at` present.
   Responses: `auth_code` exactly 8 chars of `[0-9A-Z]` (base-36, not hex), fully consumed (G15);
   unknown or missing fields -> decline (auth) or retry (CDR).
3. **Error mapping table.** Every CAUT disposition and CDRI state/code
   (`BAD_REQUEST`, `INTERNAL`, `NO_TARIFF`, `RATE_LIMITED`, HTTP 403/429,
   timeout) mapped to one action: decline, ACK, withhold ACK, or alert.
4. **Idempotency.** Stable `requested_at` per `(terminal, seq)` for CAUT;
   wrap-safe `cdr_id` for CDRI; bridge retries only idempotent calls.
5. **Timeouts and budgets.** Bridge-to-CARDMAN timeout plus mm_manager
   client timeout stay well under the terminal's 40 s
   `card_auth_required_timeout`; DNS and connect bounded.
6. **Contract tests in CI.** Stub bridge and a mock CARDMAN generated
   from the documented shapes; tests fail on any shape drift. A
   scheduled job diffs MCardSys `API_CONTRACTS.md` and `ratesrv.rexx`
   against the pinned commit and opens an issue on change.
7. **Live verification gate.** Before release, run the full chain
   against a CARDMAN dev stack (MCardSys `docker-compose`), covering
   approve, decline, `approved_limited`, `no_tariff`, CDR `MATCHED`,
   `UNMATCHED`, `DUPLICATE`, `FILTERED`, and rate sync.
8. **Observability.** Per-request correlation id (`cdr_id` / `seq`),
   masked logs, counters for declines-by-reason, CDR retries, and sync
   age; alert when CDRs are being withheld or rate sync is stale.

## 2. Decisions needed before implementation

These change the design and are the owner's call (and in some cases
CARDMAN review's call).

| ID | Question | Options | Recommendation |
|---|---|---|---|
| D1 | What happens to a CDR when the bridge or CARDMAN is unreachable? | (a) Withhold the ACK so the terminal keeps the record and re-sends next call-in. (b) ACK and keep it in a local outbox. | **(a).** (b) stores an unmasked PAN and conflicts with `SECURITY_RULES.md` rule 2; choosing it requires amending that rule first. If (a) fails on hardware, this decision goes back to the owner. Scope is narrow: CDRI durably parks `UNMATCHED`/`NOTARIFF` itself, so any well-formed CDRI response counts as delivered; only transport errors and `INTERNAL` need this. (a) depends on the terminal re-sending un-ACKed CDRs, to verify on hardware. |
| D2 | Platform support for the CARDMAN build | Linux only, or implement Winsock client | **Linux only** (static musl artifact). Windows keeps failing closed and the README says so. |
| D3 | WAPI identity group | Decided in CARDMAN 2.5.0.1: `payphone`, CN = userid = `mm-<env>` (ADR 40). | Done. |
| D4 | PINs longer than 4 digits | Restrict CARDMAN cards used here to 4-digit PINs, or find another wire path | Restrict, and have the bridge reject longer-PIN cards explicitly. Needs CARDMAN confirmation. |
| D5 | Hold sizing (no mid-call cutoff, G6) | Fixed max-duration HOLD, or per-destination | Decided in ADR 40: hold = rated 180 s estimate capped at 2000 c; overage is an accepted, uncollected `CDR_SHORTFALL` in 2.5.0.1; per-source hold policy proposed for 2.6. `approved_limited` is declined at the terminal. Option to record: cap card call length on the terminal itself (unverified on hardware). |

## 3. Phased roadmap

Each phase ends at a gate. Phases map onto, and do not replace, the
integration repo's SOW phases.

### P0. Decisions and fixtures (no runtime change)
- Resolve D1 to D4.
- Capture a real session from the user's terminal against **unmodified
  upstream** with `-l`, using a test card, and store a redacted copy as
  the first fixture.
- Gate: decisions recorded in this directory; one fixture checked in
  with no real PAN/PIN.

### P1. Card-data hardening (G2, G3, G4, G11)
- Add `MM_CARDMAN` option; redact diagnostics; loopback guard and input
  validation; explicit handling of 0x01/0x04/0x3d/0x46; CARDMAN-mode
  defaults (require `-m` or explicit test flag, require explicit DB
  path, refuse default craft access code).
- Acceptance: after a replay, a checker **decodes** each artifact (stdout,
  `-l` UART hex lines reassembled into bytes, pcap payloads, SQLite rows)
  and searches for the test PAN and PIN in both ASCII and packed-BCD
  form; neither is found. A plain text grep is not sufficient; non-loopback bridge address is
  refused; unknown auth table yields a decline, not silence.

### P1b. CARDMAN 2.5 required fixes (G14, alignment C1 to C15)

Fixes from [`CARDMAN_2.5_ALIGNMENT.md`](CARDMAN_2.5_ALIGNMENT.md), to the
contract in [`BRIDGE_CONTRACT.md`](BRIDGE_CONTRACT.md). Status: fork side
(T13) and reference bridge done on branch `claude/relaxed-mccarthy-swgc9b`,
and **live-proven** on 2026-10-13: `tools/cardman_live_test.sh` passes
against a CARDMAN 2.5.0.1 dev container (MCardSys `v2.5.0.1`, `90bcb47`)
after CARDMAN's own `scripts/verify-payphone.sh` passed. Every acceptance
item below is covered by that run except the hardware checks (BRIDGE_CONTRACT
section 4) and the port of the bridge to the integration repo (T16).
Item 1 blocks every real authorization, so this phase must land before
any live CARDMAN test (P5).

| # | Fix | Where | Task |
|---|---|---|---|
| 1 | Send `seq_num` and `requested_at` to CAUT. Keep `requested_at` identical for retries of the same `(terminal, seq)`, because CAUT derives `auth_code` from `(pan, seq_num, requested_at)`. | Fork: pass FUNF `seq` and a per-session `requested_at` on `/authorize`. Bridge: forward both to CAUT. | T13 (fork), T16 (bridge) |
| 2 | Stop rating in the bridge. CDRI rates every call (ADR 8). Send the terminal's `call_cost` as the reported `charge_cents`. | Fork: forward `call_cost` on `/cdr`. Bridge: remove `RateCache`, pass the reported value. | T13, T16 |
| 3 | Send `international` correctly, so international calls get an `INTL` hold. Also fill CAUT's `terminal` object (`telco_id`, `terminal_id`, `phone_number`, `terminal_serial`). | Fork: derive `yes` when `call_type & 0x0f == 0x06`; send the terminal phone number. Bridge: forward the flag and build the full `terminal` object. | T13, T16 |
| 4 | Bridge authenticates as group `payphone`, CN = userid = `mm-<env>`. | Created in CARDMAN 2.5.0.1 (ADR 40). | T16 |
| 5 | **Send a CDR for every approved auth, answered or not.** The earlier "skip unanswered calls" was wrong: that CDR (duration under 3 s) is the only way to release the hold. | Fork: forward every CDR with a CARDMAN `auth_code`. Bridge: `duration_secs` 0 for unanswered/abandoned/denied. | T13, T16 |
| 6 | `approved_limited` -> invalid, plus an immediate release CDR. | Bridge. | T16 |
| 7 | Expand 7-digit numbers with the payphone's NPA; send `ani`, `dnis`, `switch_id`, `source: mm_manager`. | Bridge. | T16 |
| 8 | Carry CARDMAN's base-36 `auth_code` (not hex) in the wire field. | Fork. | T13 |

- Acceptance: against a CARDMAN dev stack, approve/decline round-trip
  succeeds with no 400 from CAUT; an international dial produces an
  `INTL` hold; a retried auth within a session yields the same
  `auth_code`; a CDR's billed amount comes from CDRI, not the bridge;
  the bridge authenticates as `payphone`; an unanswered call's hold ends
  `RELEASED`; a 7-digit dial is rated as the payphone's NPA; journal
  actors are `platform:mm-<env>` / `mediation:mm-<env>`; `cdr.source` is
  `mm_manager`.
- Porting the reference bridge to the integration repo is T16 (owner
  approval). CARDMAN-side pre-work is done (2.5.0.1).

### P2. CDR durability (G1)
- Implement D1's chosen approach (withhold ACK on transport/`INTERNAL`
  failure). Idempotency via a wrap-safe `cdr_id` (alignment C11).
- Acceptance: replay with stub bridge stopped mid-session, then restart
  it; every CDR reaches the stub exactly once (by `cdr_id`).

### P3. Terminal configuration and pricing (G5, G8)
- CARDMAN card-table profile and rate-table generator.
- Acceptance: a CARDMAN test BIN always produces a 0x02 auth request on
  hardware; rate shown on the terminal display equals CDRI's
  `charged_cents` for the same call in a fixed test matrix.

### P4. Automated verification (G10)
- Register replay tests in CMake (`add_test`), run in the existing
  `release.yml` `ctest` step on Linux targets.
- Cases: approve, decline, bridge unreachable, malformed bridge JSON,
  CDR ACK behavior per D1, unknown auth table, redaction assertions.
- Acceptance: CI green on every Linux matrix entry; tests fail if
  redaction or fail-closed behavior regresses.

### P4b. Rate sync and API hardening (O1, O2)
- Implement O1 pipeline and O2 items 1 to 6 (T14, T15).
- Acceptance: as listed under O1 and O2; contract tests run in CI.

### P5. End-to-end and release
- Soft modem path per `docs/softmodem/` (ADR-0002, replaces integration
  repo SOW Phase 3's softmodem + PTY), and a live CARDMAN dev instance.
- Release artifact: `Linux-x64-static` built with `MM_CARDMAN=ON`.
- Upstream sync procedure documented (merge upstream, re-run P4).
- Gate: O2 item 7 live verification passes; full dial-in -> auth ->
  call -> CDR -> CDRI MATCHED on real hardware; integration repo graduates to a CARDMAN ADR.

## 4. Acceptance criteria (whole effort)

1. No full PAN or any PIN in stdout, logs, pcap, UDP mirror, or SQLite
   of a CARDMAN build, verified by an automated test that decodes each
   artifact and checks ASCII and packed-BCD forms.
2. Every authorization is decided by CARDMAN; any failure declines.
3. No CDR acknowledged to the terminal is lost when the bridge or
   CARDMAN is down.
4. Card data leaves the host only through the bridge's mTLS link.
5. `MM_CARDMAN=OFF` builds behave like upstream.
6. All builds stay `-Wall -Wextra -Werror` clean.

## 5. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Terminal does not re-send un-ACKed CDRs (D1a fails) | No compliant way to retain the CDR | Verify on hardware in P0. If it fails, return D1 to the owner; any local store needs a `SECURITY_RULES.md` amendment first. |
| Packed timestamp/duration encodings wrong (open item in `CDR_CONTRACT_SCOPING.md`) | Wrong charges | Confirm against a real capture in P0 before P2. |
| No open soft modem sends the Bell 212A answer tone (2225 Hz); the terminal may reject a V.22 answerer | No softmodem path | Hardware answer-tone matrix SM-1 to SM-3 (`docs/softmodem/SPEC.md` §9.2) before any reliance; hardware modem path already works and stays supported (ADR-0002 S4). |
| Upstream changes `process_mm_table()` heavily | Rebase conflicts | Keep CARDMAN logic in new files; case blocks call one function each. |
| Redaction misses a table type | Card data leak | Redactor keyed on table IDs with tests per ID; default to redacting any table containing a card-number field. |

## 6. Out of scope

Smartcard/chip (CARDMAN-side deferral), Mondex settlement beyond a safe
decline/hold, coin-call settlement, changes to the CARDMAN or
integration repos.

# CARDMAN compatibility plan

Goal: a build of this fork that can front real CARDMAN card traffic
without losing billable calls or exposing card data, while staying a
small, rebasable delta on upstream `hharte/mm_manager`.

Gap IDs (G1 to G13) refer to [`ARCHITECTURE_ANALYSIS.md`](ARCHITECTURE_ANALYSIS.md).

## 1. Target architecture

```
Nortel Millennium terminal
   |  1200 baud Bell 212A dial-in
   v
[ hardware modem ]  or  [ Asterisk app_softmodem -> TCP -> socat PTY ]   (SOW Phase 3)
   |  serial / PTY  (-f <dev> -m)
   v
mm_manager  (CARDMAN build: MM_CARDMAN=ON)
   - DLOG protocol, local config tables, local SQLite (masked only)
   - CARDMAN adapter: auth, CDR (ACK held on failure), redaction  <- fork delta
   |  loopback only (127.0.0.1 or Unix socket), HTTP/JSON
   v
cardman_bridge.py  (integration repo; holds WAPI identity + mTLS)
   |  CAUT / CDRI / GET /api/rates
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
| Rate-table generator | Script (sibling to `generate_lcd.py`) that pulls `GET /api/rates` (needs `mediation` or `admin` ACL) and emits terminal rate tables, so displayed prices match what CDRI will charge. | G8 |
| CAUT/CDRI payload completion | Forward `seq_num`, stable `requested_at`, `international` (from `call_type`), terminal phone number; on CDRs forward reported `call_cost` and skip unanswered calls. | G14 |
| Replay test harness | `.dlog` fixtures with synthetic test PANs, run in `ctest` against `tools/cardman_stub_bridge.py`. | G10 |

## 1a. Future-state objectives

### O1. Terminal rate tables loaded from CARDMAN

Today terminal rate tables are hand-built files in `tables/`; CARDMAN
rates the real charge (ADR 8). Target: CARDMAN's tariff master is the
single source, and the prices a caller sees are generated from it.

- **Source**: `GET /api/rates` (`ratesrv.rexx`, ACL `mediation,admin`,
  per-actor cap 30 req / 60 s). Returns `rates[]` rows of `telco_id`,
  `call_type`, `carrier`, `initial_cents/secs`, `second_cents/secs`,
  `addl_cents/secs`, `version`, `effective_ts`, plus `count`.
- **Pipeline**: bridge (or a separate sync job using a `mediation`
  credential) pulls on a schedule -> validates -> maps rows to terminal
  `DLOG_MT_RATE_TABLE` / `DLOG_MT_INTL_SBR_TABLE` / LCD tables with the
  existing `generate_*.py` encoders -> writes them to the per-terminal
  table directory -> mm_manager's existing `check_mm_table_is_newer()`
  download path sends them on the terminal's next call-in. No change to
  the DLOG protocol.
- **Change detection**: keep the last-seen `(telco_id, call_type,
  carrier) -> version` map; regenerate only when a version or row set
  changes.
- **Fail-safe**: a failed or suspicious pull (non-`ok`, `count`
  mismatch, zero rows, parse error) keeps the last good tables; never
  push an empty or partial table to terminals.
- **Known limit**: `/api/rates` exposes only base rows. CDRI also applies
  per-number overrides, time-of-day periods, distance bands, connection
  fees, minimum charges and surcharges (`cdrin.rexx` `RATE_COMPUTE`).
  Terminal display can approximate, not equal, the settled charge until
  CARDMAN exposes those inputs or a quote endpoint. Raise with CARDMAN
  review; record the tolerance accepted.
- **Acceptance**: changing a rate in CARDMAN (`RADM`) reaches a test
  terminal's display on its next call-in; a fixed call matrix shows
  terminal price within the accepted tolerance of CDRI `charged_cents`.

### O2. A solid CARDMAN API integration

Make the mm_manager -> bridge -> CARDMAN chain correct by contract, not
by coincidence.

1. **One written contract per hop.** mm_manager <-> bridge contract
   versioned in this repo (JSON schema for `/authorize`, `/cdr`, rate
   sync output). Bridge <-> CARDMAN pinned to MCardSys
   `docs/api/API_CONTRACTS.md` and `ratesrv.rexx` by commit.
2. **Strict validation both ways.** Requests: digits-only PAN (16),
   PIN (4), called number; `seq_num`, `requested_at` present.
   Responses: `auth_code` exactly 8 hex chars, fully consumed (G15);
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
| D3 | WAPI identity group | New group, or reuse `agi` (the only existing group with both CAUT and CDRI) | New group, but **not named `millennium`**: that is already CARDMAN's database binding name. Suggest `payphone`. CARDMAN review decides. No fork change either way. |
| D4 | PINs longer than 4 digits | Restrict CARDMAN cards used here to 4-digit PINs, or find another wire path | Restrict, and have the bridge reject longer-PIN cards explicitly. Needs CARDMAN confirmation. |
| D5 | Hold sizing (no mid-call cutoff, G6) | Fixed max-duration HOLD, or per-destination | CARDMAN already sizes holds from its tariff engine, and `approved_limited` caps them; overage becomes an AUDIT `SHORTFALL`. Document in the CARDMAN ADR that this front end cannot honor a cap mid-call. |

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

### P1b. CAUT/CDRI payload completion (G14)
- Fork side of [`CARDMAN_2.5_ALIGNMENT.md`](CARDMAN_2.5_ALIGNMENT.md); the
  bridge side is owner action in the integration repo.
- Acceptance: against a CARDMAN dev stack, approve/decline round-trip
  succeeds (no 400 from CAUT); an international dial produces an `INTL`
  hold; a retried auth within a session yields the same `auth_code`.

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
- Integration repo SOW Phase 3 (softmodem + PTY) and a live CARDMAN dev
  instance.
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
| `app_softmodem` cannot demodulate this terminal | No softmodem path | Hardware modem path already works; softmodem is optional for compatibility. |
| Upstream changes `process_mm_table()` heavily | Rebase conflicts | Keep CARDMAN logic in new files; case blocks call one function each. |
| Redaction misses a table type | Card data leak | Redactor keyed on table IDs with tests per ID; default to redacting any table containing a card-number field. |

## 6. Out of scope

Smartcard/chip (CARDMAN-side deferral), Mondex settlement beyond a safe
decline/hold, coin-call settlement, changes to the CARDMAN or
integration repos.

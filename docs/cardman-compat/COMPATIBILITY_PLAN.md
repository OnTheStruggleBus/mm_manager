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
   - CARDMAN adapter: auth, CDR, outbox, redaction        <- fork delta
   |  loopback only (127.0.0.1 or Unix socket), HTTP/JSON
   v
cardman_bridge.py  (integration repo; holds WAPI identity + mTLS)
   |  CAUT / CDRI / GET /api/rates
   v
CARDMAN (BRICKS)
```

Design rules:

1. **Decisions about money live in CARDMAN.** mm_manager never approves
   locally and never computes the settled charge.
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
| `MM_CARDMAN` CMake option | Builds the bridge client, enables redaction and outbox, disables raw-byte diagnostics. Default ON in this fork's release workflow, OFF reproduces upstream. | G2, G9 |
| CDR outbox | SQLite table holding the **unmasked** CDR fields needed for CDRI until the bridge returns success; encrypted at rest or kept only until delivery, then deleted. A drain step runs at session end and on startup. | G1 |
| Diagnostic redactor | One function that, given a decoded packet, zeroes card-number and PIN bytes of table 0x01/0x02 before `print_mm_packet`, pcap, UDP, and UART log writes. | G2 |
| Bridge address guard | Reject non-loopback `CARDMAN_BRIDGE_ADDR` unless an explicit override is set; optional Unix-domain socket. Validate PAN/PIN/called number are digits before building JSON. | G3 |
| Unknown-auth guard | Explicit `case` for 0x01/0x04/0x3d/0x46: parse by struct size where known, decline any auth request, log and ACK-hold CDR-like records. | G4 |
| CARDMAN card-table profile | A documented `tables/cardman/` card table with CARDMAN BIN ranges, `CARD_VF_NCCVAL_IND` set (manager validation required). | G5 |
| Rate-table generator | Script (sibling to `generate_lcd.py`) that pulls `GET /api/rates` via the bridge and emits terminal rate tables, so displayed and settled prices match. | G8 |
| Replay test harness | `.dlog` fixtures with synthetic test PANs, run in `ctest` against `tools/cardman_stub_bridge.py`. | G10 |

## 2. Decisions needed before implementation

These change the design and are the owner's call (and in some cases
CARDMAN review's).

| ID | Question | Options | Recommendation |
|---|---|---|---|
| D1 | What happens to a CDR when the bridge is down? | (a) Withhold the ACK so the terminal keeps the record and re-sends next call-in. (b) ACK and keep it in a local outbox. | **(a) first, (b) as backup.** (a) keeps the terminal as the system of record and adds no local unmasked storage. It depends on the terminal re-sending un-ACKed CDRs, which must be verified on real hardware. If it does not, use (b). |
| D2 | Platform support for the CARDMAN build | Linux only, or implement Winsock client | **Linux only** (static musl artifact). Windows keeps failing closed and the README says so. |
| D3 | WAPI identity group | New `millennium` group or reuse `agi` | Defer to integration repo / CARDMAN review (already raised in `bridge/README.md`). No fork change either way. |
| D4 | PINs longer than 4 digits | Restrict CARDMAN cards used here to 4-digit PINs, or find another wire path | Restrict, and have the bridge reject longer-PIN cards explicitly. Needs CARDMAN confirmation. |
| D5 | Hold sizing (no mid-call cutoff, G6) | Fixed max-duration HOLD, or per-destination | CARDMAN side; document the constraint in the CARDMAN ADR. |

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
- Acceptance: grep of stdout, `-l` log, pcap, and SQLite after a replay
  finds no full test PAN and no PIN; non-loopback bridge address is
  refused; unknown auth table yields a decline, not silence.

### P2. CDR durability (G1)
- Implement D1's chosen approach. Add idempotent retry (the bridge
  already uses `cdr_id = mm-<terminal>-<seq>`).
- Acceptance: replay with stub bridge stopped mid-session, then restart
  it; every CDR reaches the stub exactly once (by `cdr_id`).

### P3. Terminal configuration and pricing (G5, G8)
- CARDMAN card-table profile and rate-table generator.
- Acceptance: a CARDMAN test BIN always produces a 0x02 auth request on
  hardware; rate shown on the terminal display equals the bridge's
  `charge_cents` for the same call in a fixed test matrix.

### P4. Automated verification (G10)
- Register replay tests in CMake (`add_test`), run in the existing
  `release.yml` `ctest` step on Linux targets.
- Cases: approve, decline, bridge unreachable, malformed bridge JSON,
  CDR ACK behavior per D1, unknown auth table, redaction assertions.
- Acceptance: CI green on every Linux matrix entry; tests fail if
  redaction or fail-closed behavior regresses.

### P5. End-to-end and release
- Integration repo SOW Phase 3 (softmodem + PTY) and a live CARDMAN dev
  instance.
- Release artifact: `Linux-x64-static` built with `MM_CARDMAN=ON`.
- Upstream sync procedure documented (merge upstream, re-run P4).
- Gate: full dial-in -> auth -> call -> CDR -> CDRI MATCHED on real
  hardware; integration repo graduates to a CARDMAN ADR.

## 4. Acceptance criteria (whole effort)

1. No full PAN or any PIN in stdout, logs, pcap, UDP mirror, or SQLite
   of a CARDMAN build, verified by automated test.
2. Every authorization is decided by CARDMAN; any failure declines.
3. No CDR acknowledged to the terminal is lost when the bridge or
   CARDMAN is down.
4. Card data leaves the host only through the bridge's mTLS link.
5. `MM_CARDMAN=OFF` builds behave like upstream.
6. All builds stay `-Wall -Wextra -Werror` clean.

## 5. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Terminal does not re-send un-ACKed CDRs (D1a fails) | Must store unmasked CDR data locally | Fall back to D1b with delete-on-delivery; verify on hardware in P0. |
| Packed timestamp/duration encodings wrong (open item in `CDR_CONTRACT_SCOPING.md`) | Wrong charges | Confirm against a real capture in P0 before P2. |
| `app_softmodem` cannot demodulate this terminal | No softmodem path | Hardware modem path already works; softmodem is optional for compatibility. |
| Upstream changes `process_mm_table()` heavily | Rebase conflicts | Keep CARDMAN logic in new files; case blocks call one function each. |
| Redaction misses a table type | Card data leak | Redactor keyed on table IDs with tests per ID; default to redacting any table containing a card-number field. |

## 6. Out of scope

Smartcard/chip (CARDMAN-side deferral), Mondex settlement beyond a safe
decline/hold, coin-call settlement, changes to the CARDMAN or
integration repos.

# Task cards: CARDMAN compatibility

Derived from `docs/cardman-compat/COMPATIBILITY_PLAN.md`. Each card is
sized for one PR. Do them in order unless the owner says otherwise.
Cards marked **needs decision** must not start until the named decision
is recorded.

Status: all cards **not started** as of 2026-10-13.

---

### T1. Record P0 decisions
- **Goal**: write the owner's answers to D1 to D5 into
  `docs/cardman-compat/DECISIONS.md`.
- **Files**: docs only.
- **Done when**: each decision has an answer, a date, and who made it.

### T2. Redacted session fixture
- **Goal**: one `.dlog` playback fixture containing at least one 0x02
  auth request and one 0x35 CDR, using a test card and dummy PIN.
- **Files**: `tests/fixtures/` (new).
- **Done when**: fixture replays with `-f`; a check confirms no
  non-test PAN appears in it.
- **Note**: capture against unmodified upstream on the owner's hardware
  or construct synthetically; the owner supplies the capture.

### T3. `MM_CARDMAN` build option
- **Goal**: CMake option gating the bridge client and CARDMAN-only
  behavior; default ON in this fork's release workflow.
- **Files**: `CMakeLists.txt`, `src/mm_manager.c` (call sites under
  `#ifdef`), `.github/workflows/release.yml`.
- **Done when**: both ON and OFF build clean on all CI targets; OFF
  reproduces upstream auth behavior.

### T4. Diagnostic redaction (G2)
- **Goal**: zero PAN/PIN bytes before `-v` dumps, pcap, and UDP; disable
  `-l` UART log and `-u` in CARDMAN builds (or log a redacted decode).
- **Files**: `src/mm_cardman_redact.c` (new), hooks in `src/mm_proto.c`,
  `src/mm_serial.c`, `src/mm_manager.c` option parsing.
- **Done when**: replaying T2 with `-vvv -l -p` produces no test PAN or
  PIN in any output, checked by decoding each artifact in ASCII and
  packed-BCD form (automated in T9).

### T5. Bridge address guard and input validation (G3, G15)
- **Goal**: refuse non-loopback `CARDMAN_BRIDGE_ADDR` unless an explicit
  override env var is set; validate digit-only PAN/PIN/called number
  before building JSON; require `auth_code` to be exactly 8 hex chars
  (fully consumed, no overflow) before approving; decline on any
  validation failure.
- **Files**: `src/mm_cardman_bridge.c`.
- **Done when**: unit tests cover loopback, non-loopback, override,
  and bad-input cases, including `auth_code` values `bogus`, `""`,
  `123`, `A1B2C3D4X`, and `FFFFFFFFF`.

### T6. Unknown auth/CDR table handling (G4)
- **Goal**: explicit cases for 0x01, 0x04, 0x3d, 0x46: decline any auth
  request, keep parsing in sync, never silently approve.
- **Files**: `src/mm_manager.c`, possibly structs in `src/mm_manager.h`
  (+ `mm_packtest.c` asserts).
- **Needs research**: struct layouts for 0x01/0x04/0x3d/0x46 from the
  protocol reference linked in `README.md`. Do not guess sizes; if
  unknown, end processing of the packet safely and log.
- **Done when**: replay with a synthetic 0x01 request yields a decline.

### T7. CDR durability (G1) - **needs decision D1**
- **Goal**: no CDR is lost when the bridge or CARDMAN is down, by
  withholding the ACK on transport/`INTERNAL` failure. Do not add a
  local unmasked outbox unless the owner has amended `SECURITY_RULES.md`.
- **Files**: `src/mm_manager.c` (`DLOG_MT_CALL_DETAILS`,
  `DLOG_MT_END_DATA`).
- **Done when**: replay with the stub bridge stopped, then restarted,
  delivers every `cdr_id` exactly once.

### T8. CARDMAN-mode defaults (G11)
- **Goal**: in CARDMAN builds require `-m` or an explicit test flag,
  an explicit DB path, and a non-default craft access code.
- **Files**: `src/mm_manager.c`.
- **Done when**: starting with defaults prints a clear error and exits.

### T9. Replay tests in CI (G10)
- **Goal**: `add_test` entries running playback against the stub
  bridge: approve, decline, unreachable, malformed JSON, redaction grep,
  CDR delivery.
- **Files**: `CMakeLists.txt`, `tests/` (new scripts), stub bridge
  extended with a `--malformed` mode.
- **Done when**: tests run in the existing `ctest` CI step on Linux
  targets and fail if fail-closed or redaction regresses.

### T10. CARDMAN card-table profile (G5)
- **Goal**: card table with CARDMAN BIN ranges and
  `CARD_VF_NCCVAL_IND` set, plus a doc on installing it.
- **Files**: `tables/cardman/`, `docs/cardman-compat/`.
- **Needs**: CARDMAN BIN ranges from the owner.
- **Done when**: verified on hardware that a CARDMAN test card triggers
  a 0x02 auth request.

### T11. Rate-table generator (G8) - superseded by T14
- Folded into T14, which covers the full sync pipeline.

### T13. Complete the CAUT/CDRI payload (G14) - do before T7
- **Goal**: fork side of `docs/cardman-compat/CARDMAN_2.5_ALIGNMENT.md`.
  On `/authorize` send `seq_num` (FUNF `seq`), `requested_at` (fixed per
  `(terminal, seq)` within a session), `international` (`yes` when
  `call_type & 0x0f == 0x06`), and the terminal phone number. On `/cdr`
  send the terminal's `call_cost` as the reported charge, skip
  `CALL_TYPE_UNANSWERED`, and make `cdr_id` wrap-safe by adding the
  record's `start_ts`.
- **Files**: `src/mm_cardman_bridge.[ch]`, `src/mm_manager.c` (two case
  blocks), `tools/cardman_stub_bridge.py` (assert new fields).
- **Depends on**: matching bridge change, T16 (integration repo, owner).
- **Plan**: phase P1b items 1 to 3.
- **Done when**: stub bridge asserts the fields; against a CARDMAN dev
  stack CAUT returns no 400 and an international dial gets an `INTL` hold.

### T14. Load terminal rate tables from CARDMAN (objective O1)
- **Goal**: scheduled sync from `GET /api/rates` to per-terminal rate,
  international SBR, and LCD tables, delivered by the existing table
  download on next call-in. Keep last good tables on any failed or
  suspicious pull.
- **Files**: `generate_cardman_rates.py` (new, reusing
  `generate_intl_sbr.py`/`generate_lcd*.py` encoders), sync job docs,
  `tables/cardman/`.
- **Needs**: a `mediation` (or `admin`) WAPI credential held by the sync
  job, not by mm_manager; pre-work items 2 and 3 (rate data that
  matches billing, `/api/rates` failing closed).
- **Done when**: O1 acceptance passes; unit tests cover version-change
  detection, empty/partial response rejection, and row-to-table mapping.

### T15. API integration hardening (objective O2)
- **Goal**: O2 items 1 to 6: versioned JSON schemas for the
  mm_manager <-> bridge contract, strict request/response validation,
  error-mapping table implemented and tested, bounded timeouts, contract
  tests in CI, scheduled drift check against MCardSys `API_CONTRACTS.md`
  and `ratesrv.rexx`.
- **Files**: `docs/cardman-compat/contract/` (schemas), `tests/`,
  `src/mm_cardman_bridge.c`, `tools/cardman_stub_bridge.py`, a workflow.
- **Depends on**: T5, T13.
- **Done when**: contract tests fail on any field rename/removal on
  either hop; every CAUT disposition and CDRI state has a test.

### T16. Bridge changes for CARDMAN 2.5 (plan P1b items 1 to 4) - **owner approval required**
- **Repo**: `OnTheStruggleBus/-mm-cardman-integration`, not this one. Do
  not start without the owner's go-ahead and repo access.
- **Goal**: in `bridge/cardman_bridge.py`, send `seq_num`,
  `requested_at`, `international`, and the full `terminal` object to
  CAUT; remove `RateCache` and send the reported `charge_cents` to CDRI;
  treat CDRI `INTERNAL` and transport errors as retryable and every other
  state as delivered; use the WAPI group from pre-work item 1 (not
  `millennium`).
- **Depends on**: T13 (fork sends the new fields); pre-work item 1
  (`docs/cardman-compat/PREWORK.md`) for the group.
- **Done when**: the bridge's mock CARDMAN enforces CAUT's required
  fields and the bridge passes; a run against a CARDMAN dev stack meets
  P1b acceptance.

### T12. Linux-only CARDMAN release and docs (G9, D2)
- **Goal**: README states CARDMAN mode is Linux-only; release workflow
  labels the CARDMAN artifact.
- **Files**: `README.md`, `.github/workflows/release.yml`.

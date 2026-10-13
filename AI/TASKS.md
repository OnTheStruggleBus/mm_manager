# Task cards: CARDMAN compatibility

Derived from `docs/cardman-compat/COMPATIBILITY_PLAN.md`. Each card is
sized for one PR. Do them in order unless the owner says otherwise.
Cards marked **needs decision** must not start until the named decision
is recorded.

Status as of 2026-10-13: T13 done (offline-tested; live run pending) and
the T16 reference bridge done in `tools/`; all other cards not started.

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

### T13. Complete the CAUT/CDRI payload (G14) - **done, live run pending**
- **Done**: `src/mm_cardman.[ch]` (new glue), `src/mm_cardman_bridge.[ch]`,
  call sites in `src/mm_manager.c`. `/authorize` sends `seq_num`, a
  `requested_at` stable per `(terminal, seq)` for 600 s, `call_type`,
  `international`, the line number. `/cdr` sends every CDR carrying a
  CARDMAN `auth_code` (answered or not), `call_cost`, `call_type`,
  `rate_type`, terminal id and line number, with `cdr_id`
  `mm-<terminal>-<seq>-<start_ts>` (UTC). `auth_code` is base-36, not hex.
- **Tests**: `mm_cardman_test` (ctest), `mm_cardman_drive` +
  `tools/test_cardman_bridge.py` (mock CARDMAN).
- **Remaining**: `tools/cardman_live_test.sh` against a CARDMAN 2.5.0.1
  dev container; on hardware, confirm the terminal echoes the full 64-bit
  `auth_code` and emits a CDR for an approved call that never connects.

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

### T16. Bridge for CARDMAN 2.5.0.1 - **reference done here; port needs owner approval**
- **Reference**: `tools/cardman_bridge.py`, tested by
  `tools/test_cardman_bridge.py`; contract in
  `docs/cardman-compat/BRIDGE_CONTRACT.md` section 3.
- **Port tasks for `OnTheStruggleBus/-mm-cardman-integration`
  `bridge/cardman_bridge.py`** (do not start without the owner's go-ahead):
  1. Identity: group `payphone`, cert CN = Basic-Auth userid = `mm-<env>`;
     password from env only. Remove the `millennium` group suggestion.
  2. CAUT body: `variant:"funf"`, `seq_num`, `requested_at` (keep it per
     `(terminal, seq, card)` for 600 s), `international` `"yes"`/`"no"`
     from the fork (never `"unknown"`), full `terminal` object.
  3. Dispositions: only `approved` is valid; `approved_limited` is
     invalid **and** triggers a zero-second release CDR; non-200,
     timeout, unparseable -> invalid.
  4. Normalize numbers: 7 digits -> terminal NPA + 7; 0/1+7 -> NPA+7;
     0+10 -> 10.
  5. CDRI body: `cdr_id` from the fork unchanged, `auth_code`, `pan`,
     `charge_cents` = terminal cost or 0, `start_ts`, `duration_secs` 0
     for unanswered/abandoned/denied, `called_number`, `ani` = line,
     `dnis`, `switch_id` = terminal, `source:"mm_manager"`.
  6. Delete `RateCache` and every `GET /api/rates` call (403 for
     `payphone`).
  7. Answer mm_manager `{"delivered":bool}` on `/cdr`: true for any CDRI
     state (`DUPLICATE`, `NOTARIFF` included), false for transport
     errors, `INTERNAL`, and 400.
  8. Retry once with the identical body after a transport error, within
     a 9 s budget; release the hold if the approval cannot be written
     back to mm_manager.
  9. Listen on loopback only; never log PAN or PIN.
  10. Update `bridge/tests/mock_cardman.py` to enforce `seq_num`/
      `requested_at` and the base-36 `auth_code`.
- **Done when**: `tools/cardman_live_test.sh` passes against the ported
  bridge.

### T12. Linux-only CARDMAN release and docs (G9, D2)
- **Goal**: README states CARDMAN mode is Linux-only; release workflow
  labels the CARDMAN artifact.
- **Files**: `README.md`, `.github/workflows/release.yml`.

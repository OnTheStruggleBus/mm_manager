# Soft modem implementation plan

- **Status**: Draft, 2026-10-27. Nothing is built yet.
- **Implements**: [ADR-0002](ADR-0002-SOFT-MODEM.md) and [`SPEC.md`](SPEC.md).
  Section numbers below (§n) refer to `SPEC.md` unless another file is named.
- **Does not change**: `src/`, the card path, ADR-0001's planes.

This plan turns SPEC §10's phases (SM-P0 to SM-P4) into task cards, each
sized for one PR, in an order that front-loads the unknowns. Section 2
lists what a review of the SPEC against today's code confirmed, and the
gaps that the cards below close.

## 1. What gets built, and where

| Deliverable | Where | Owner | Licence |
|---|---|---|---|
| `mm-softmodem-dte` shim (`serve`, `connect`) | `tools/mm_softmodem_dte.py` | this repo | MIT |
| Shim event spool writer and validator (§4.5) | `tools/mm_softmodem_events.py` | this repo | MIT |
| MODEM table tool (DLOG `0x21`, §3.1a, T-7) | `tools/mm_modem_table.py` | this repo | MIT |
| Tests T-2, T-6, T-7 (CI) | `tools/test_mm_softmodem_dte.py`, `tools/test_mm_modem_table.py`, `CMakeLists.txt` | this repo | MIT |
| Deployment examples: systemd units, tmpfiles.d, nftables, Asterisk and datamodem snippets (no secrets) | `deploy/softmodem/` (new) | this repo | MIT |
| Lab scripts for T-1, T-3 to T-5, and the leak decoder (rule 9) | `tools/softmodem_lab/` | this repo | MIT |
| U2 patch (Bell 2225 Hz answer) | a datamodem fork, **not** this repo (ADR-0002 S3, S6) | owner's fork | GPL-3.0+ / LGPL-2.1 |
| Hardware checks SM-0 to SM-10 | `docs/cardman-compat/HARDWARE_TEST_PLAN.md` §4 | this repo (docs) | - |

datamodem itself is never vendored, built or fetched by this repo or its
CI (SPEC §8, acceptance item 7).

## 2. Review of the SPEC against the code (2026-10-27, `main` @ `f89aa30`)

### 2.1 Confirmed

Each SPEC claim about mm_manager below was checked in `src/`:

- **Init strings get through the shim.** `send_at_command()`
  (`src/mm_modem.c`) does `tcflush(TCIOFLUSH)`, writes the command plus
  CR, sleeps 100 ms, then calls `wait_for_modem_response(…, 5)`. That
  function reads one line at a time, ending at CR **or** LF. So the reply
  `\r\nOK\r\n` uses three of its five tries (`\r`, `\n`, `OK\r`). The
  shim must not add more blank lines than that. The default init string
  contains spaces and fits §4.1's `^[Aa][Tt][\x20-\x7e]{0,78}$`.
- **`RING` followed 100 ms later by `CONNECT` cannot be lost.** After
  `RING`, `mm_connection_wait()` loops back into
  `wait_for_modem_response()`, whose `drain_serial()` is `tcdrain()`, an
  output-only wait (`src/mm_serial_posix.c`), not a flush.
- **termios on a PTY.** `platform_init_serial()` does `cfmakeraw()` and
  then sets `CRTSCTS | HUPCL`, with `VMIN=0` and `VTIME=10`. It ignores
  `tcsetattr()`'s return value. The `TIOCMGET`/`TIOCMSET` ioctls behind
  the DTR hang-up are also unchecked. So on a PTY these are no-ops, as
  §4.1 says. T-6 proves it on the CI host.
- **Hang-up path.** `proto_disconnect()` always calls `hangup_modem()`
  (DTR drop, plus a 1 s sleep). It runs on carrier loss, on a read
  error, and when the terminal sends `FLAG_DISCONNECT`
  (`src/mm_proto.c`). On a normal session end, mm_manager is back in
  `wait_for_modem_response()` about 1 s after the terminal's disconnect
  packet.
- **The `COOLDOWN` window.** `receive_mm_packet()` gives up after
  `PKT_TIMEOUT_MAX` (10) reads of 1 s each, and the main loop allows 3
  retries, so about 30 s, as §4.1 says.

### 2.2 Gaps to close while building

| ID | Gap | Resolution (card) |
|---|---|---|
| P-1 | **PTY master semantics on Linux are assumed, not checked.** §4.2 detects "no reader" through `POLLHUP` on the master. Linux reports `POLLHUP` (and `read()` gives `EIO`) whenever no slave fd is open. While the slave is closed, a naive `selectors` loop would spin at 100% CPU. What happens before the slave is first opened is not stated anywhere. | SM-T2 starts with a 20-line probe test that records the exact behaviour on the CI kernel. The loop then unregisters the master on `POLLHUP` and re-polls it every 250 ms until a reader reappears. T-2 asserts that CPU use stays bounded while no reader is attached. |
| P-2 | **PTY slave ownership.** `/dev/pts/N` is created owned by the shim's user, with group `tty` and mode 0620. C-4 needs group `mm-dte` and mode 0660. | The shim runs `fchown(slave, -1, gid(mm-dte))` and `fchmod(0660)` at start, and refuses to start if either fails. `mm-modem` is a member of `mm-dte`. The symlink directory `/run/mm-softmodem` is 0750 `mm-modem:mm-dte`; `sock/` stays 0700. tmpfiles.d sets this up (SM-T7). |
| P-3 | **The spool file format is unspecified**, for the shim and for the bridge alike (`wapi-bridge/SPEC.md` §3.2 item 4 says only "append one event"). | Propose one file per event, `<id>.json`, holding a full VOCABULARY §5 envelope. It is written to `<spool>/.tmp/` and then `rename()`d into place, so the relay never reads half a file. The size cap is counted in files. Record the format in `wapi-bridge/SPEC.md` §9 so W1 uses the same one (SM-T3). |
| P-4 | **There is no machine-readable PIV schema yet.** §4.5 says each event is "checked against the PIV schema before it is written". | Until T15's versioned schemas exist, the shim gets a closed allow-list validator for its own two types: exact key set, enum membership, types, and no string longer than 32. It also rejects any digit run of 13 or more characters (a stricter local twin of the relay's Luhn scan). Replace it with the shared schema when T15 lands (SM-T3). |
| P-5 | **Python tests are not in `ctest` today.** `tools/test_cardman_bridge.py` runs by hand. The `alpine:3.20` CI container has no `python3`. macOS has no `SO_PEERCRED`. Windows has no PTY. | Add `find_package(Python3 COMPONENTS Interpreter)`, and register the shim tests only `if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND Python3_FOUND)`. They then run on `Linux-x64-glibc` and are skipped elsewhere, which is logged (SM-T1). |
| P-6 | **`COOLDOWN` also follows a normal call.** mm_manager is ready about 1 s after a clean end (2.1), but the shim cannot see the terminal's disconnect packet without parsing the payload, which it must not do. So every call costs 35 s of busy time. | Accept it: call-ins are minutes apart, and a refused terminal retries. Measure it in SM-6 and SM-9. The fallback in §4.1 (build without `USE_MODEM_DTR`) stays in reserve. No card. |
| P-7 | **Nothing consumes the events until ADR-0001 W6 (relay) is built, and the missed-call-in alarm needs F1 (W2).** | The shim writes its spool from day one. Until W6 exists, the operator watches the shim's INFO log and each instance's "Disconnected" lines (the interim already given in §4.5). SM-P2's alarm check uses the spool, not a delivered webhook. |
| P-8 | **Card-data path size.** §3.4 sets "under 300 lines including the argument parser" for the shim. A spool writer and validator would eat that budget. | Put them in a separate module, `mm_softmodem_events.py`. It never sees line bytes, and its interface takes only S1 values. That keeps the byte-handling file small enough to review in one sitting. |

## 3. Decisions needed before code (SM-P0)

| ID | Recommendation in ADR-0002 | Blocks |
|---|---|---|
| DS-1 | Accept GPL-3.0+ datamodem at a process boundary | SM-P2 onwards (nothing in SM-P1) |
| DS-2 | Python stdlib shim | SM-T2, SM-T3 |
| DS-3 | Prepare U2 ourselves, offer it upstream | SM-TU |
| DS-4 | el-modem for SM-3 only, x86-64 | SM-T9 (SM-3 branch only) |
| DS-5 | Per-terminal MODEM table as an interim | SM-T6's use in the field (the tool itself can be built first) |

Plan-level choices, recommended here and listed so the owner can
override them: the `deploy/softmodem/` directory (P-2, SM-T7); the
spool file format (P-3); and one file for the shim plus one for its
events (P-8).

## 4. Task cards

The order runs top to bottom unless a dependency says otherwise. Cards
marked **CI** must be green on `Linux-x64-glibc` before merge. Every card
that touches the shim goes through the `SECURITY_RULES.md` checklist,
because the shim carries the full PAN and PIN.

### SM-T0. Record DS-1 to DS-5
- **Files**: `docs/softmodem/ADR-0002-SOFT-MODEM.md` (owner decisions
  table: answer, date, who).
- **Done when**: each DS has an answer. ADR status moves to Accepted (or
  stays Proposed with a reason).

### SM-T1. Python tests in ctest (P-5) **CI**
- **Goal**: a ctest hook for Python tests on Linux, skipped elsewhere.
  Also register the existing `tools/test_cardman_bridge.py` if it passes
  unattended (it should, since it uses a mock CARDMAN).
- **Files**: `CMakeLists.txt`.
- **Done when**: `ctest` on Linux-glibc runs the Python tests. Alpine,
  macOS and Windows configure with a status message saying they are
  skipped. No new CI job is needed.

### SM-T2. Shim core: `serve` and `connect` (§3.4, §4.1 to §4.4, T-2) **CI**
- **Depends on**: SM-T0 (DS-2), SM-T1.
- **Step 1 (spike, kept as a test)**: probe the Linux PTY master's
  `POLLHUP`/`EIO` behaviour before the first slave open, with a slave
  open, and after the slave closes (P-1).
- **Design**:
  - One `selectors` loop. Explicit state `IDLE`/`RINGING`/`ONLINE`/
    `COOLDOWN`. Deadlines are held as monotonic timestamps (idle timer,
    cooldown, 100 ms `RING`->`CONNECT`, 1 s pre-`NO CARRIER` silence),
    and each select timeout is the nearest one.
  - Listener: `socket(AF_UNIX)`, `bind`, `chmod 0600`; on accept, check
    `SO_PEERCRED` uid. Reads the header bounded at 128 bytes, with a 2 s
    deadline (a peer that never sends a header must not pin `RINGING`).
    Refusals map 1:1 to `refusal_reason` (VOCABULARY §6.6), and ends to
    `call_end_reason` (§6.7).
  - PTY: `os.openpty()`, `tty.setraw(slave)`, P-2 ownership, atomic
    symlink (`symlink` to a temp name, then `rename`). The shim's own
    slave fd is closed after setup so that P-1 detection works.
  - `ONLINE` copy: `os.read(fd, 4096)` -> `sendall` and back, with no
    buffering beyond one read and no inspection of bytes.
  - Logging: a fixed set of INFO messages only (§4.4). No `%r`/`%s` of
    any buffer anywhere. A test greps the module's source for logging
    calls with buffer variables.
  - Signals: SIGTERM -> end call (`shutdown`) -> unlink socket and
    symlink -> exit 0. SIGHUP is ignored (systemd restarts the service
    instead).
  - `connect`: header from `MODEM_RATE`/`MODEM_MODULATION`, then a
    bidirectional copy until either side hits EOF. Exits non-zero if the
    socket is refused, so datamodem clears the call.
- **Tests (T-2)**: every case listed in SPEC §9.1 T-2, plus the P-1
  bounded-CPU case, the header-deadline case, and "no payload in the
  logs": run a call carrying the test PAN `4012888888881881` and a dummy
  PIN through the shim with logging captured at DEBUG, then decode the
  log per rule 9.
- **Done when**: T-2 is green in CI. `mm_softmodem_dte.py` stays under
  300 lines. The security checklist has been reviewed.

### SM-T3. Event spool (§4.5, P-3, P-4) **CI**
- **Depends on**: SM-T2 (the hooks where events are emitted).
- **Files**: `tools/mm_softmodem_events.py`, tests, and a short section
  in `docs/wapi-bridge/SPEC.md` §9 recording the shared spool format.
- **Design**: `emit_refused(modem_id, reason)` and
  `emit_ended(modem_id, reason, duration_secs, dte_silent)` are the only
  entry points. Neither can take bytes. The id is sortable: 48-bit ms
  time plus 80 random bits, in Crockford base32. `source` is
  `urn:piv:mm-softmodem-dte:<deployment>` and `subject` is
  `modem/<modem_id>`. The validator follows P-4. When the spool is full,
  or a write fails, the event is dropped and a counter is incremented.
  The counter goes into the INFO log every hour. It never raises into
  the call path.
- **Done when**: unit tests cover both types, every enum value, each
  validator rejection (extra key, bad enum, long digit run), full-spool
  drop, and a crash between write and rename (the `.tmp` leftover is
  ignored and cleaned at start).

### SM-T4. Real mm_manager against the shim (T-6) **CI**
- **Depends on**: SM-T2.
- **Files**: `tools/test_mm_softmodem_dte.py` (an integration class) or
  `tests/softmodem_t6.py`, plus an `add_test` that depends on the
  `mm_manager` target.
- **Design**: start `serve` in a temp dir and run the built
  `mm_manager -m -w -f <pty> -n …` in its own temp working directory (so
  that `mm_manager.db` stays local). A fake caller on the socket plays
  terminal-side frames from a synthetic fixture (test PAN only; reuse
  T2's fixture when it exists, or else frame a minimal
  `DLOG_MT_CALL_IN` with a correct CRC). Assertions are made on
  mm_manager's stdout ("Modem initialized.", "Connected!",
  "Disconnected", "Waiting…"). Then end the call abruptly, wait out the
  cooldown, and place a second call.
- **Done when**: green in CI in under 90 s (the cooldown and timeouts
  can be shortened with flags for the test only; mm_manager's own ~30 s
  window cannot, so budget for it).

### SM-T5. MODEM table tool (§3.1a, T-7) **CI**
- **Depends on**: none (it can run in parallel with SM-T2).
- **Files**: `tools/mm_modem_table.py`, `tools/test_mm_modem_table.py`.
- **Design**: named fields, in the MSR 2.1 pp. 2-279 to 2-289 order,
  encode to and decode from the 35-byte file. `--answer-tone-not-required`
  sets bit 2 of byte 33. Output is written as temp plus `rename` in the
  target directory, and takes `tables/<tid>/.lock` if it exists (F5).
  The tool refuses to write under `tables/default/` (DS-5). The bit
  order stays marked "inferred" until SM-1a.
- **Done when**: T-7 passes. Defaults are byte-identical to
  `tables/default/mm_table_21.bin`, and "not required" differs only at
  byte 33 (`0x02` -> `0x06`).

### SM-T6. Shim CLI and operator surface
- Fold into SM-T2 unless SM-T2 grows too large. The arguments are
  `--pty-link`, `--socket`, `--modem-id`, `--spool`, `--idle-timeout`
  (40), `--cooldown` (35) and `--dte-group`. There is no flag that
  enables payload logging (§4.4).

### SM-T7. Deployment examples (§3.2, §3.3, §3.6, §3.7, §7)
- **Depends on**: SM-T2 (paths and flags).
- **Files**: `deploy/softmodem/` with these files:
  - `mm-softmodem-dte@.service`
  - `datamodem@.service`, with `UnsetEnvironment=DATAMODEM_RECORD`,
    `LimitCORE=0`, `User=mm-modem`, and the password from
    `LoadCredential=`
  - `mm-manager@.service` (instances `soft` and `bell`)
  - `tmpfiles.d/mm-softmodem.conf`
  - `nftables-mm-modem.nft` (owner-match egress)
  - `datamodem.conf.example`
  - `pjsip.conf.example`, `extensions.conf.example`
  - `README.md`
- **Lint test (CI)**: a small Python check that every unit sets
  `LimitCORE=0`, that the datamodem unit unsets `DATAMODEM_RECORD`, that
  `log-level` is not above `info`, and that no example contains a
  password. This locks C-2, C-5 and C-7 against later edits.
- **Done when**: the lint test is green. The README walks through §3.6's
  migration steps against these files.

### SM-T8. Lab bring-up (SM-P2: T-1, T-3, T-4, T-5)
- **Depends on**: SM-T0 (DS-1), SM-T2, SM-T3, SM-T7.
- **Files**: `tools/softmodem_lab/` with a T-3 pattern sender and echo
  reader, a T-4 permission and routing checklist script, and
  `leak_decode.py`. `leak_decode.py` reassembles logs, journald export,
  spool and capture files, then searches for the test PAN and PIN in
  ASCII and packed BCD. Share it with card T9 (replay tests) rather than
  writing two decoders.
- **Done when**: T-1 and T-3 to T-5 pass on the lab host. A forced
  registration loss raises the host-monitoring alarm. A refusal appears
  as a valid spool event. Results go in a lab log in
  `docs/softmodem/` (no card data).

### SM-TU. U2 Bell 212A answer (critical path, in parallel from SM-T0)
- **Depends on**: DS-3.
- **Where**: a fork of billsimon/datamodem, never this repo.
- **Work**: port cryan209/v90modem's LGPL-2.1 `v22bis_tx.c`/
  `v22bis_rx.c` changes (ADR-0002 #15), keeping the LGPL headers. Add an
  option such as `--answer-tone bell`. Extend `datamodem selftest` with
  a Bell 212A originator. Offer the patch upstream, and the spandsp half
  to freeswitch/spandsp.
- **Done when**: the patch is offered and a fork build is available for
  SM-3. Track it with an issue link in ADR-0002.

### SM-T9. Hardware session (SM-P3: SM-0 to SM-10)
- **Depends on**: SM-T5, SM-T8; for SM-5, a way to make test cards
  (`wapi-bridge/SPEC.md` §14.1).
- **Files**: `docs/cardman-compat/HARDWARE_TEST_PLAN.md` §4 (copy §9.2
  in), plus results.
- **Order**: SM-0 -> SM-1 -> SM-1a (decides the bit; fix SM-T5 if it is
  wrong) -> SM-1b -> SM-2 -> SM-3 only if needed -> SM-4 to SM-10.
- **Stop rule**: if the answer-tone matrix fails, stop. The hardware
  modem stays the supported path (§10).

### SM-T10. Close-out docs (SM-P4)
- `THREAT_MODEL.md`: the four sink rows and the boundary 1 refinement
  (§7).
- `COMPATIBILITY_PLAN.md` §1 and §5: final state.
- `README.md`: a section "Using a soft modem", pointing to
  `deploy/softmodem/`.
- `AI/REPO_GUIDE.md`: add the new `tools/` and `deploy/` rows.
- `AI/TASKS.md`: mark the SM cards done.

## 5. Sequencing

```
SM-T0 ──┬─> SM-T1 ──> SM-T2 ──┬─> SM-T3 ──┐
        │                     ├─> SM-T4   ├─> SM-T7 ──> SM-T8 ──> SM-T9 ──> SM-T10
        │   SM-T5 ────────────┼───────────┘                 ^
        └─> SM-TU (fork, critical path) ────────────────────┘ (SM-3 only)
```

| Milestone | Cards | SPEC gate |
|---|---|---|
| M1 "shim in CI" | SM-T1, SM-T2, SM-T3, SM-T4, SM-T5 | SM-P1: T-2, T-6, T-7 green; security checklist passed |
| M2 "lab chain" | SM-T7, SM-T8 | SM-P2 |
| M3 "on hardware" | SM-T9 (+ SM-TU build for SM-3) | SM-P3 |
| M4 "documented" | SM-T10 | SM-P4 |

Cross-plan dependencies (ADR-0001): the spool format (P-3) should be
agreed before W1 writes the bridge's spool. Event delivery waits for W6.
The missed-call-in alarm waits for F1 and W2 (P-7). Nothing in M1 or M2
waits for ADR-0001.

## 6. Test map

| Test | Card | Runs in |
|---|---|---|
| T-2 shim unit, P-1 probe, log-leak decode | SM-T2 | CI (Linux-glibc) |
| Spool and validator | SM-T3 | CI |
| T-6 real mm_manager | SM-T4 | CI |
| T-7 table round trip | SM-T5 | CI |
| Unit-file lint | SM-T7 | CI |
| T-1, T-3, T-4, T-5 | SM-T8 | Lab host |
| SM-0 to SM-10 | SM-T9 | Hardware |
| Regression: hardware modem `-m` without `-w` (acceptance 6) | SM-T9 | Hardware |

## 7. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| The terminal rejects a V.22 answerer (SM-1, SM-1b and SM-2 fail) | Soft modem unusable without U2 | SM-TU in parallel from day one. The stop rule keeps the cost bounded: M1's code is small, and SM-T5 is useful on its own. |
| The byte-33 bit order is wrong | SM-1b is a false negative | SM-1a proves the bit with a hardware modem first |
| PTY behaviour differs from the assumptions (P-1) | Busy loop, or missed `no_reader` | Probe test first, in SM-T2 |
| The cooldown cuts capacity (P-6) | A second terminal gets busy for 35 s after each call | Measure in SM-6 and SM-9. Run one shim set per simultaneous call-in (§3.3). `+++`/`ATH` fallback in reserve. |
| datamodem upstream stalls | No U1, U2 or U3 | The contract is plain bytes, so any `--exec`-capable modem fits. The U2 patch lives in our fork. |
| A leak through a new sink (spool, journald) | Rule 4 breach | The P-4 validator, the closed `emit_*` API, the T-5/SM-8 decode check over the spool, and the unit-file lint |

## 8. Done

SPEC §12 acceptance items 1 to 7, with M1 to M4 complete. The first PR
after this plan is SM-T1 plus the P-1 probe from SM-T2, which settles
the one assumption the shim design depends on before the shim is
written.

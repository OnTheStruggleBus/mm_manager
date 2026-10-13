# Architecture analysis: mm_manager fork vs. CARDMAN requirements

Reviewed at fork commit `3e173a3` (`main`). Line numbers refer to that
commit and will drift; grep for the quoted symbol if they do.

## 1. What mm_manager is

A single C program (plus ~25 table-editing CLI tools) that acts as the
"Millennium Manager" for Nortel Millennium payphones. The terminal
always dials in (1200 baud, Bell 212A); the manager never calls out.
Over that session the terminal uploads status/CDR/auth tables and the
manager downloads configuration tables (card, carrier, rate, LCD,
NPA-NXX, display prompts, etc.).

### Module map

| Layer | Files | Role |
|---|---|---|
| Entry / dispatch | `src/mm_manager.c` | CLI parsing, main loop (`main`, ~L300-630), per-table `switch` in `process_mm_table()` (~L652-1265), table download (`mm_download_tables()`), parameter-table generators. |
| Wire structs | `src/mm_manager.h` | `PACKED` structs for every DLOG table, table-ID enum (L96-210). Sizes asserted in `src/mm_packtest.c`. |
| Protocol | `src/mm_proto.c` | Packet framing, CRC, ACK/retry, sequence bits, pcap/UDP mirroring. |
| Transport | `src/mm_connection.c`, `src/mm_serial*.c`, `src/mm_modem.c` | Modem AT handling, serial I/O (POSIX/Win32), or file playback of a recorded byte log. |
| Persistence | `src/mm_accounting.c`, `src/mm_sqlite3.c`, `third-party/sqlite3.c` | Local SQLite `mm_manager.db`: TAUTH, TCDR, TALARM, TCASHST, TOPCODE, etc. |
| Helpers | `src/mm_util.c` | BCD/phone-number conversion, `mask_pan()` (L109), table pretty-printers. |
| CARDMAN client (fork) | `src/mm_cardman_bridge.[ch]` | Hand-rolled HTTP/JSON client: `POST /authorize`, `POST /cdr` to `CARDMAN_BRIDGE_ADDR` (default `127.0.0.1:8420`). |
| Test stub (fork) | `tools/cardman_stub_bridge.py` | Approve-all / decline-all stub of the bridge. |
| Offline tools | `src/mm_card.c`, `mm_rate.c`, `mm_lcd.c`, `generate_*.py`, `html/admess_editor.html` | Build/edit the binary tables in `tables/`. |

### Runtime model

- One process, one serial port (or one playback file), one terminal
  session at a time. Strictly single-threaded.
- `main()` loops on `mm_connection_wait()` then repeatedly calls
  `process_mm_table()` until carrier drops or 3 consecutive failures
  (`mm_manager.c` ~L602-627).
- Each received packet may carry several tables; replies are
  accumulated in `ack_payload` and sent once per packet. CDR ACKs are
  batched until `DLOG_MT_END_DATA` when a multi-table upload is in
  progress.
- Database path is hard-coded to `mm_manager.db` in the working
  directory (L587). `test_mode` defaults to `TRUE` (L359) and is cleared
  only by `-m`.

## 2. What the fork already changed (PRs #1 to #6)

| Change | Where | Status |
|---|---|---|
| Auth decision delegated to bridge, fail closed | `case DLOG_MT_FUNF_CARD_AUTH` (L1134-1225), `cardman_bridge_authorize()` | Done; tested against stub and mock CARDMAN, not against a live DLOG session. |
| CDR submitted to bridge | `case DLOG_MT_CALL_DETAILS` (L776-854), `cardman_bridge_submit_cdr()` | Done; same test caveat. |
| `auth_code` LE64 fix on CDR path | L790-795 | Done. |
| PAN masked `first6****last4` in TAUTH/TCDR + stdout | `mm_accounting.c` L67, L185 | Done. |
| PIN dropped from TAUTH, craft PIN redacted in TOPCODE | `mm_accounting.c` ~L100-110, L459-470 | Done. |
| Static musl Linux release target | `.github/workflows/release.yml` | Done. |

## 3. Data flows

### Authorization (DLOG_MT_FUNF_CARD_AUTH, 0x02)

```
terminal --0x02 (PAN, PIN, dialed#, service code)--> process_mm_table()
  -> mm_acct_save_TAUTH()            masked PAN, no PIN -> SQLite + stdout
  -> cardman_bridge_authorize()      full PAN + PIN, plaintext HTTP -> bridge
       bridge -> CARDMAN CAUT (mTLS)
  <- {approved, auth_code hex}       hex -> low 32 bits of wire uint64
  -> DLOG_MT_AUTH_RESP_CODE (0x41)   resp_code 0 = valid, 1 = declined
```

The bridge call is synchronous inside the protocol loop. The terminal's
own wait budget is generous: `generate_user_if_parameters()` sets
`card_auth_required_timeout = 4000` (40 s) and
`card_validation_timeout = 4500` (45 s), in 10 ms units (L1909, L1921).
The bridge client uses a 3 s `SO_RCVTIMEO`/`SO_SNDTIMEO`
(`mm_cardman_bridge.c` L23, L67). `getaddrinfo()` has no timeout, and
`connect()` honoring `SO_SNDTIMEO` is Linux behavior, not portable.

### CDR (DLOG_MT_CALL_DETAILS, 0x35)

```
terminal --0x35 (seq, PAN, dialed#, start ts, duration, auth_code)--> process_mm_table()
  -> mm_acct_save_TCDR()             masked PAN -> SQLite
  -> cardman_bridge_submit_cdr()     result ignored
  -> DLOG_MT_CDR_DETAILS_ACK         always sent (now or with END_DATA)
```

## 4. Gaps against CARDMAN requirements

Severity: **B** = blocks real-money use, **H** = high, **M** = medium.

| # | Sev | Finding | Evidence |
|---|---|---|---|
| G1 | B | **CDRs can be lost.** The CDR ACK is sent whether or not the bridge accepted the record. Once ACKed, the terminal treats the record as delivered. A bridge or CARDMAN outage during upload means the call never settles, and the only copy left is the masked local TCDR row, which cannot be resubmitted because the PAN is masked. | `mm_manager.c` L842 (return value discarded), L844-852 (ACK unconditional). |
| G2 | B | **Raw PAN/PIN still leaves the process through diagnostics.** `-v` hex-dumps every packet to stdout; `-l` writes every UART byte to a file; `-p` writes a pcap; `-u` mirrors packets over UDP. All carry the FUNF auth table with full PAN and PIN. Phase 1 masking only covered `mm_accounting.c`. | `mm_proto.c` L89, L96-98, L343-346, L461-464; `mm_serial.c` L105-118; options at `mm_manager.c` L457, L485, L505, L514. |
| G3 | H | **Bridge link is plaintext and unrestricted.** Full PAN + PIN go as JSON over plain HTTP. `CARDMAN_BRIDGE_ADDR` accepts any host, so a misconfiguration sends card data across a network in clear. No escaping or digit validation before building JSON with `snprintf`. | `mm_cardman_bridge.c` L160, L235-237. |
| G4 | H | **Unhandled auth/CDR table types.** Only 0x02 auth and 0x35 CDR are handled. 0x01 `CARD_AUTH_REQ` (legacy), 0x04 `CD_CALL_DETAILS`, 0x3d `COIN_CALL_DETAILS`, 0x46 `MONDEX_DEPOSIT_REC` fall to `default:`, which advances the parse pointer by one byte and sends no reply. That desyncs parsing of the rest of the packet and leaves an auth request unanswered (terminal behavior on that path unverified). | `mm_manager.c` L1248-1251; enum `mm_manager.h` L96-153. |
| G5 | H | **Terminal routing of CARDMAN cards is not specified.** Whether the terminal asks the manager at all is set by the downloaded card table's `vfy_flags` (`CARD_VF_NCCVAL_IND`, `CARD_VF_IMMEDIATE_AUTH_IND`). No CARDMAN card-table profile exists, so a CARDMAN BIN could be accepted locally by the terminal without ever reaching CARDMAN. | `src/mm_card.h` L111-119; tables under `tables/`. |
| G6 | H | **No mid-call balance control is possible.** `dlog_mt_auth_resp_code_t` carries only `resp_code` and a 64-bit `auth_code` (22 bytes, asserted in `mm_packtest.c` L84). There is no credit or time limit field. CARDMAN must reserve a HOLD sized for the longest permitted call and settle on the CDR. | `mm_manager.h` L807-812. |
| G7 | M | **PIN width.** The wire PIN is a `uint16_t` (four BCD digits). CARDMAN PINs longer than 4 digits cannot be carried on this path. | `mm_manager.h` `dlog_mt_funf_card_auth_t`; `mm_manager.c` L1174-1175. |
| G8 | M | **Displayed rate vs. settled rate.** Terminal rate/LCD tables are local files; the bridge rates from `GET /api/rates` with a placeholder algorithm (`RateCache.compute_charge_cents`, per integration repo `bridge/README.md`). Customers can be shown one price and charged another. | `tables/`, integration repo `bridge/`. |
| G9 | M | **Windows builds decline every card.** The `_WIN32` bridge client is a fail-closed stub. Correct per policy, but a Windows release is unusable for CARDMAN. | `mm_cardman_bridge.c` L293-312. |
| G10 | M | **No automated test of the CARDMAN path.** CI runs `ctest` but nothing is registered for the bridge client or the auth/CDR handlers. README documents `.dlog` playback (`-f <file>` without `-m`), which is the natural fixture mechanism. | `CMakeLists.txt`; `release.yml` L182; `README.md` L1310. |
| G11 | M | **Insecure defaults for a CARDMAN deployment.** Hard-coded craft access code and key card (`mm_manager.c` L345-354), telco ID `VZ`, `test_mode` on unless `-m`, DB path fixed to CWD. | `mm_manager.c` L345-359, L581-587. |
| G12 | M | **Single-line scaling.** One process per modem line. Multiple lines need multiple processes, each with its own working directory (DB + terminal tables). There is no shared state, which is fine, but it must be documented and supervised. | `mm_manager.c` L587, main loop. |
| G13 | Low | **Doc drift.** Integration repo cites "154 table types" (its `claude.md`) while the fork README and proposal count 78 defined; SOW refers to branch `phase-2-cdr-wiring`, but the work is merged on `main`. Recorded here only; the integration repo is not edited from this fork. | Integration repo `claude.md`, `SOW.md`. |

## 5. What is already right and should be kept

- The integration-repo decision to patch a few call sites rather than
  wrap the binary holds up: the CARDMAN touchpoints really are two
  `case` blocks plus accounting sinks.
- Fail-closed authorization is implemented correctly for every error
  path in `cardman_bridge_authorize()`.
- Keeping WAPI credentials and mTLS in a separate bridge process means
  the C process never holds CARDMAN secrets.
- The delta against upstream `hharte/mm_manager` is small and isolated,
  which keeps upstream rebases cheap. The plan preserves that.

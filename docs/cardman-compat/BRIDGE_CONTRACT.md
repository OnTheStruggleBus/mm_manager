# Bridge contract (mm_manager <-> bridge <-> CARDMAN 2.5.0.1)

Source of truth for the CARDMAN side: MCardSys `v2.5.0.1` (`main` at
`90bcb47`), [`docs/frontends/MILLENNIUM_GATEWAY_CONTRACT.md`](https://github.com/OnTheStruggleBus/MCardSys/blob/main/docs/frontends/MILLENNIUM_GATEWAY_CONTRACT.md),
[ADR 40](https://github.com/OnTheStruggleBus/MCardSys/blob/main/docs/ai/ARCHITECTURE_DECISIONS.md#adr-40-millennium-payphone-gateway),
`docs/api/API_CONTRACTS.md` (CAUT, CDRI, `GET /api/rates`) and
`scripts/verify-payphone.sh`. Where this file and those disagree, those win.

Implemented by `src/mm_cardman.c`, `src/mm_cardman_bridge.c` (fork) and
the reference bridge `tools/cardman_bridge.py`.

## 1. Audit of the fork and bridge before this change (task 1)

Fork at `7cffead`; integration bridge at `687898c`
(`-mm-cardman-integration/bridge/cardman_bridge.py`).

| # | Gap | Where | Fixed by |
|---|---|---|---|
| A1 | `/authorize` sends no `seq_num`, `requested_at`, call type or line number. CAUT answers 400 without the first two. | `src/mm_cardman_bridge.c` L315-317 | fork sends all four |
| A2 | `auth_code` decoded as hex (`strtoul(..., 16)`). CARDMAN codes are 8 chars of `[0-9A-Z]` (`auth.rexx` `DERIVE_AC`), so almost every real code decodes wrong and its CDR can never match the hold. | `src/mm_cardman_bridge.c` L341-345; `src/mm_manager.c` L816 (`%08X`) | base-36 codec, full 41-bit value in the 64-bit wire field |
| A3 | A too-long JSON string value is truncated, not rejected, so `A1B2C3D4X` would pass as `A1B2C3D4`. No check of the code's format (G15). | `src/mm_cardman_bridge.c` L188-191, L335-346 | reject on overflow; exactly 8 `[0-9A-Z]` |
| A4 | Bridge HTTP status never checked; a non-200 with an `"approved":true` body would approve. | `src/mm_cardman_bridge.c` L294-301 | 200 required |
| A5 | Client timeout (3 s) equals CAUT's route timeout, so a slow approval reaches the bridge after mm_manager gave up: terminal declined, hold left open. | `src/mm_cardman_bridge.c` L103 | 10 s client; bridge retries inside 9 s |
| A6 | No digit validation before fields go into JSON (G3 partial). | `src/mm_cardman_bridge.c` L315, L355-363 | allow-lists per field |
| A7 | `cdr_id` = `mm-<terminal>-<seq>`, collides when the 16-bit `seq` wraps (C11). | `src/mm_manager.c` L815 | `mm-<terminal>-<seq>-<start_ts>` |
| A8 | `start_ts` labelled `Z` but is the terminal's local time (the terminal clock is set from `localtime_r`, `mm_manager.c` `DLOG_MT_TIME_SYNC_REQ`). Wrong rate period off-UTC hosts. | `src/mm_manager.c` L820-823 | `mktime` -> UTC |
| A9 | Every CDR goes to the bridge, including coin calls and cards CARDMAN never authorized (their PAN would reach CARDMAN). | `src/mm_manager.c` L842 | forward only CDRs with a CARDMAN `auth_code`, answered or not |
| A10 | CDR forwards no `call_cost`, `call_type`, `rate_type`, line number. | `src/mm_cardman_bridge.c` L354-364 | all forwarded |
| A11 | `call_cost` (two `uint32_t`) byte-swapped with `LE16`: wrong on big-endian hosts. | `src/mm_manager.c` L789-790 | `LE32` |
| A12 | CDR ACK sent whatever the bridge says (G1). | `src/mm_manager.c` L844-852 | **open**, task T7 / decision D1 |
| B1 | Bridge sends `international: "unknown"` always; no `seq_num`/`requested_at`; `terminal` has only `terminal_id`. | integration `cardman_bridge.py` `authorize()` | reference bridge |
| B2 | Bridge maps `approved_limited` to approved. ADR 40 says invalid. | `_handle_authorize` | invalid + release CDR |
| B3 | Bridge rates the call itself (`RateCache`) from `GET /api/rates`, which the `payphone` identity cannot call (403) and which ADR 8 makes moot. | `RateCache`, `_handle_cdr` | removed; reported charge only |
| B4 | No 7-digit expansion, no `ani`/`dnis`/`switch_id`, no zero duration for unanswered calls (ring time would be billed or a hold left open). | `_handle_cdr` | reference bridge |
| B5 | No retry; no release of a hold whose approval could not be delivered. | whole file | identical-body retry; release CDR |

## 2. mm_manager -> bridge

Loopback HTTP/1.1, JSON, one request per connection. Anything but HTTP
200 with the documented body is a failure. mm_manager waits 10 s.

### `POST /authorize`

```json
{"pan":"9149000000000011","pin":"2468","service_code":"101",
 "called_number":"5551212","terminal_id":"2125551000","phone_number":"2125551000",
 "seq_num":4211,"requested_at":"2026-10-13T18:42:55Z","call_type":85,"international":"no"}
```

- `called_number`: digits as dialed. The bridge normalizes.
- `terminal_id` / `phone_number`: mm_manager identifies a terminal by its
  own line number, so both carry that number.
- `requested_at`: real UTC time at the first sight of `(terminal, seq)`,
  reused for 600 s (`cardman_requested_at()`), never `mm_time()` (fixed
  in test mode).
- `international`: `"yes"` iff `call_type & 0x0F == 0x06`.

Response: `{"approved":true,"disposition":"approved","auth_code":"0Q7KX2LM"}`
or `{"approved":false,"disposition":"<CAUT disposition or null>"}`.
mm_manager answers the terminal "valid" only for `approved:true` with an
8-char `[0-9A-Z]` code; it stores the code's base-36 value in the wire
`auth_code`.

### `POST /cdr`

Sent only for CDRs whose `auth_code` is non-zero.

```json
{"cdr_id":"mm-2125551000-17-2026-10-13T18:43:00Z","auth_code":"0Q7KX2LM",
 "pan":"9149000000000011","called_number":"5551212","start_ts":"2026-10-13T18:43:00Z",
 "duration_secs":95,"call_type":85,"rate_type":0,"call_cost_collected":0,
 "call_cost_requested":175,"terminal_id":"2125551000","phone_number":"2125551000","seq":17}
```

Response: `{"delivered":true,"state":"MATCHED",...}` when CARDMAN owns the
record (any CDRI state, `DUPLICATE` and `NOTARIFF` included), else
`{"delivered":false,"retryable":<bool>}`. Today mm_manager only logs it;
withholding the ACK on `delivered:false` is task T7.

## 3. Bridge -> CARDMAN

Identity: group `payphone`, cert CN = Basic-Auth userid = `mm-<env>`
(dev: `mm-dev`). Routes: CAUT and CDRI only; everything else, including
`GET /api/rates`, is 403.

**CAUT body**: `variant:"funf"` (the terminal collects a PIN), `pan`,
`pin`, `service_code`, normalized `called_number`, `international`
(`"yes"` or `"no"`, never `"unknown"`), `seq_num`, `requested_at`, and
`terminal{telco_id, terminal_id, phone_number, terminal_serial:""}`
(accepted, not stored by 2.5.0.1).

**Number normalization**: 7 digits -> terminal NPA + 7; `0`/`1` + 7 ->
NPA + 7; `0` + 10 -> 10; 10 and `1`+10 unchanged; international (call
type or `011` prefix) unchanged.

**Dispositions**: `approved` -> valid. `approved_limited` -> invalid, and
the bridge immediately sends a release CDR (`cdr_id`
`mm-<terminal>-A<seq>-<requested_at>`, `duration_secs` 0) -> `FILTERED`,
hold `RELEASED`. Every other disposition, a non-200, a timeout or an
unparseable body -> invalid.

**CDRI body**: `cdr_id`, `auth_code`, `pan`, `charge_cents` (terminal
`call_cost_requested`, else `call_cost_collected`, else 0; stored, not
billed), `start_ts`, `duration_secs` (0 when the call type is unanswered,
abandoned or denied), normalized `called_number`, `ani` = the terminal's
line number, `dnis` = the normalized called number, `switch_id` =
terminal id, `source:"mm_manager"`.

**Retries**: one retry of the identical body after a transport error,
within a 9 s budget. CAUT is idempotent on `(pan, seq_num,
requested_at)`; the bridge also keeps `requested_at` per `(terminal, seq,
HMAC(card))` for 600 s in memory, so a retry after an mm_manager restart
still matches. CDRI is idempotent on `cdr_id`.

**Lost approval**: if the bridge cannot write an approval back to
mm_manager, it releases the hold. It cannot detect mm_manager timing out
after the write succeeded; the 10 s client timeout makes that unlikely,
not impossible (see `CARDMAN_REQUESTS.md` R1).

## 4. Known limits (accepted in 2.5.0.1)

- **No mid-call cutoff.** Hold = rated estimate for 180 s, capped at
  2000 c. Overage is an uncollected `CDR_SHORTFALL`, even on a card with
  funds. The bridge does not work around it. Option to record: the
  terminal's own call-length limits (rate table / `limserv` timers) could
  cap card calls near the hold; unverified on hardware.
- **Wire `auth_code` width.** The code needs 42 bits of the 64-bit field.
  That the terminal echoes all 64 bits back in the CDR is unverified on
  hardware (upstream only ever printed the field).
- **Unanswered calls need a CDR.** That the terminal produces a CDR
  (with the `auth_code`) for an approved call that never connects is
  unverified on hardware. If it does not, the hold stays open (R1).

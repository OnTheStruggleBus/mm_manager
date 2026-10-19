# Hardware test plan (real Millennium terminal + CARDMAN 2.5.0.1)

What still has to be proven on a physical terminal. The software path
(fork glue -> bridge -> CARDMAN) is live-proven against a CARDMAN
2.5.0.1 container by `tools/cardman_live_test.sh`, but that run uses
`mm_cardman_drive`, which **builds the terminal's records itself**. So
anything about what the terminal actually sends, or how it behaves, is
still an assumption. This plan turns those into checks.

To run it with step-by-step help in a Claude Code session, use the
prompt in [`HARDWARE_TEST_SESSION.md`](HARDWARE_TEST_SESSION.md).

Contract: [`BRIDGE_CONTRACT.md`](BRIDGE_CONTRACT.md). Open hardware
items: its section 4. Task status: [`../../AI/TASKS.md`](../../AI/TASKS.md)
T10, T13, T7.

## Before the session

**Card-data safety (`AI/SECURITY_RULES.md`):**

- Use **only cards issued by the CARDMAN under test** (`POST /api/newcust`
  as the issuer). Never a real customer card.
- Do **not** run mm_manager with `-v`, `-l`, `-p` or `-u`. Those
  diagnostics still write full card numbers and PINs (rule 4; redaction
  is task T4, not done). If one is needed to debug, treat its output as
  card data: keep it off shared storage and delete it after the session.
- Never paste a full card number or PIN into notes, tickets or chat. Use
  the masked form `first6****last4`.

**Setup:**

1. CARDMAN 2.5.0.1 running, `./scripts/seed.sh` done, and
   `./scripts/verify-payphone.sh` passing. If it fails, stop: that is a
   CARDMAN problem.
2. `tools/cardman_live_test.sh` passing against that CARDMAN, so the
   software path is known good before the terminal is involved.
3. The bridge running on the mm_manager host, loopback only:

   ```bash
   CARDMAN_BRIDGE_PASSWORD="$PP_PASS" python3 tools/cardman_bridge.py \
     --listen 127.0.0.1:8420 --cardman-url https://<cardman>:8443 \
     --client-cert client-payphone.pem --client-key client-payphone-key.pem \
     --ca-cert ca.pem --user mm-dev --telco-id TELCO01 2> bridge.log
   ```

4. mm_manager started with `CARDMAN_BRIDGE_ADDR=127.0.0.1:8420` (the
   default), with stdout and stderr both kept as `mm_manager.log`
   (`2>&1`): the CARDMAN glue logs its failures on stderr.
5. Two or three fresh test cards, each with its PIN, and a note of the
   terminal's line number (it is the `terminal_id`, `switch_id` and
   `ani` in CARDMAN). **Blocked as of 2026-10-19:** `NEWC` issues the
   number and PIN, but there is no way yet to put the number on a card
   the terminal can read. Options are listed in
   [`../wapi-bridge/SPEC.md`](../wapi-bridge/SPEC.md) §14.1.

**Evidence for every test:** the `auth_code` (from `bridge.log`, line
`authorize: ... auth_code=XXXXXXXX`), the matching lines in
`mm_manager.log` and `bridge.log`, and the CARDMAN rows below.

## Queries

Run against the CARDMAN database (`millennium`). In the dev container:

```bash
docker exec -it <container> bash -c 'cd /app; cnf() { sed -nE "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*(.*[^[:space:]])[[:space:]]*$/\1/p" bricks/conf/bricks.cnf | head -1; }; PGPASSWORD="$(cnf db_password)" psql -h "$(cnf db_host)" -p "$(cnf db_port)" -U "$(cnf db_user)" -d millennium'
```

```sql
-- Q1: the hold for one authorization
SELECT auth_code, state, amount_cents, called_number, created_ts FROM holds WHERE auth_code = 'XXXXXXXX';

-- Q2: its journal (actors must be platform:mm-<env> / mediation:mm-<env>)
SELECT type, actor, amount_cents FROM journal WHERE auth_code = 'XXXXXXXX' ORDER BY 1;

-- Q3: the call record CARDMAN received for it
SELECT cdr_id, state, called_number, duration_secs, charge_cents, reported_charge_cents,
       to_char(start_ts AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS start_utc,
       source, switch_id, ani
FROM cdr WHERE auth_code = 'XXXXXXXX';

-- Q4: everything the terminal sent this session
SELECT cdr_id, auth_code, state, duration_secs FROM cdr
WHERE switch_id = '<terminal line>' AND received_ts > now() - interval '2 hours' ORDER BY received_ts;

-- Q5: holds still open for the test cards (should be none at the end)
SELECT auth_code, amount_cents, created_ts FROM holds
WHERE state = 'OPEN' AND created_ts > now() - interval '2 hours';
```

## 0. Prerequisites: do these first

Nothing else can pass until both do.

| # | Test | Do | Pass when | If it fails |
|---|---|---|---|---|
| 0.1 | Terminal asks the host | Insert a CARDMAN test card, dial any number. | `mm_manager.log` shows a card auth request and `bridge.log` an `authorize:` line. | The card table does not send that BIN to the host: task T10 (card-table profile). |
| 0.2 | Terminal collects a PIN | Same call. | The terminal prompts for a PIN before the auth goes out. | Without a PIN, CAUT `funf` declines every call. Card-table setting, T10. |

## 1. Wire format: things only the terminal can confirm

| # | Test | Do | Pass when | If it fails |
|---|---|---|---|---|
| 1.1 | PIN byte order | One call with the right PIN, one with a wrong PIN, same card. | Right PIN: `disposition=approved`, terminal connects. Wrong PIN: `disposition=declined`, terminal refuses. | Right PIN declined means the glue decodes the terminal's PIN bytes wrongly (`cardman_authorize_funf`, the `%04x` swap). Fork bug. |
| 1.2 | Full `auth_code` comes back in the CDR (open item) | Any approved, answered call. Nearly every CARDMAN code needs more than 32 bits, so any approval tests it. | Q3 `state` = `MATCHED`, Q1 `state` = `CAPTURED`. | `UNMATCHED`, or mm_manager logs "auth_code CARDMAN never issued": the terminal does not echo all 64 bits. Needs a different encoding of the code on the wire; record it in `BRIDGE_CONTRACT.md` section 4. |
| 1.3 | 7-digit local dial | Dial a 7-digit number. | Q1/Q3 `called_number` = terminal's area code + the 7 digits. | Check the digits in `bridge.log`; normalization is in `cardman_bridge.py` `normalize_number`. |
| 1.4 | 1 + 10 digits | Dial 1 + area code + number. | Q3 `called_number` as dialed, `state` `MATCHED`. | As 1.3. |
| 1.5 | Toll-free | Dial an 800 number. | Approved; CARDMAN rates it `TOLLFREE`. | As 1.3. |
| 1.6 | International | Dial `011` + a country number. | Q1 hold sized at INTL rates (in the dev seed, 125c, not 300c); `bridge.log` shows the call type with low nibble 6. | Domestic hold: the terminal's call type for this call is not `0x?6`. Record the real value; the fork's `cardman_call_type_is_international` and the bridge need it. |
| 1.7 | Start time | Note the wall-clock time an answered call starts. | Q3 `start_utc` is that moment in UTC (within the terminal's clock drift). | Off by whole hours: the terminal clock's zone differs from the mm_manager host's (`cardman_terminal_ts_to_utc`). |

## 2. The two open behaviour checks

| # | Test | Do | Pass when | If it fails |
|---|---|---|---|---|
| 2.1 | Approved call that never connects (open item) | Three calls: ring with no answer; a busy number; hang up during ringing. | For each, a CDR arrives with the `auth_code`; Q3 `state` = `FILTERED`; Q1 `state` = `RELEASED`; Q5 empty. | No CDR: the hold stays `OPEN` for good (CARDMAN never expires one). Record which case; it strengthens request R1 in `CARDMAN_REQUESTS.md`. |
| 2.2 | CDR while the bridge is down (input to decision D1) | Steps below. | Recorded either way: this test gathers evidence, it does not pass or fail. | |

Test 2.2 needs care, because a stopped bridge also refuses the auth. Do it in this order:

1. With the bridge **running**, start an approved call and keep it up.
2. **Stop the bridge** while the call is in progress, then hang up.
3. Note what mm_manager logs (`CDR ... not delivered to CARDMAN`) and
   that it still ACKs the CDR to the terminal (current behaviour, gap
   G1).
4. Restart the bridge and let the terminal call in again.

Record: does the terminal ever resend that CDR? With today's code it
should not, since mm_manager ACKed it, so the CDR is lost and its hold
stays `OPEN` (Q5). That confirms the loss task T7 exists to fix.

5. **Optional, needs a test build:** repeat with a build that withholds
   the CDR ACK when the bridge did not accept it. Record whether the
   terminal keeps the CDR and resends it on a later call-in, how many
   times, and whether it raises an alarm or goes out of service. This
   is the evidence decision D1 needs: withholding the ACK is only safe
   if the terminal reliably resends.

## 3. Normal flows on real hardware

| # | Test | Do | Pass when |
|---|---|---|---|
| 3.1 | Answered call | A call of about 1-2 minutes. | Q1 `CAPTURED`; Q2 shows `HOLD` by `platform:mm-<env>` and `CAPTURE` by `mediation:mm-<env>`; Q3 `source` `mm_manager`, `switch_id` = `ani` = the terminal line. Note `charge_cents` against `reported_charge_cents`: they differ wherever the terminal's rate tables do not match CARDMAN's tariff (expected until T14). |
| 3.2 | Call longer than the hold | Stay connected for more than 3 minutes. | Q3 `charge_cents` = the hold amount, and CARDMAN logs a `CDR_SHORTFALL` for the rest (known, accepted loss). Note whether any terminal timer ends the call. |
| 3.3 | Bridge down | Stop the bridge, try a card call. | The terminal refuses the card within its auth timeout (about 40 s), not hangs. No hold (Q5). |
| 3.4 | CARDMAN slow or down | Stop CARDMAN (or block port 8443), try a card call. | Refused, as 3.3. `bridge.log` shows the CAUT failure. |
| 3.5 | Several CDRs in one call-in | Make two or three card calls, then let the terminal upload together. | Each arrives once (Q4); none `UNMATCHED`. |
| 3.6 | Coin call | A coin call. | Nothing reaches CARDMAN (no `bridge.log` line, nothing in Q4). |
| 3.7 | Card the terminal validates itself | If the card table has a locally validated card type, use one. | Nothing reaches CARDMAN: no `auth_code`, so mm_manager does not forward it. |
| 3.8 | Same card, back to back | Two quick calls with one card. | Two holds, two different `auth_code`s, both settle. |

## Wrap-up

- Q5 must be empty. Release any open hold left by the session with a
  zero-second CDR, or ask the CARDMAN operator to.
- Record results in the table below. Then update `BRIDGE_CONTRACT.md`
  section 4 (the two open items), `AI/TASKS.md` T10 / T13 / T7, and
  `CARDMAN_REQUESTS.md` if CARDMAN needs anything.
- Delete any diagnostic capture made during the session.

## Results

| # | Date | Terminal / firmware | Result | Evidence (auth_code, Q-row values) | Notes |
|---|---|---|---|---|---|
| 0.1 | | | | | |
| 0.2 | | | | | |
| 1.1 | | | | | |
| 1.2 | | | | | |
| 1.3 | | | | | |
| 1.4 | | | | | |
| 1.5 | | | | | |
| 1.6 | | | | | |
| 1.7 | | | | | |
| 2.1 | | | | | |
| 2.2 | | | | | |
| 3.1 | | | | | |
| 3.2 | | | | | |
| 3.3 | | | | | |
| 3.4 | | | | | |
| 3.5 | | | | | |
| 3.6 | | | | | |
| 3.7 | | | | | |
| 3.8 | | | | | |

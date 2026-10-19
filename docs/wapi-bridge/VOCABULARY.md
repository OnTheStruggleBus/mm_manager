# Payphone Interchange Vocabulary (PIV) v1

Status: **Proposed** (2026-10-18), part of
[`ADR-0001-WAPI-BRIDGE.md`](ADR-0001-WAPI-BRIDGE.md).

One set of names, types, units and enumerations for card, call,
terminal, tariff and terminal-configuration information. It is used:

- as the field dictionary behind every hop in this fork (mm_manager <->
  bridge, bridge <-> CARDMAN, config API, events);
- as the language other apps and front ends use to exchange this
  information with us or with each other (webhooks, exports, a future
  Tranz gateway, the Asterisk AGI, reporting tools).

PIV is a **vocabulary, not a new contract**. The live contracts stay
what they are: CARDMAN `docs/api/API_CONTRACTS.md` for WAPI and
`docs/cardman-compat/BRIDGE_CONTRACT.md` for the mm_manager hop. PIV
maps their wire names to one canonical name each. If PIV and a CARDMAN
contract disagree about a CARDMAN field, the CARDMAN contract wins and
PIV is corrected.

## 1. Rules

1. **Adopt, don't invent.** Where CARDMAN already names a concept, the
   canonical name is CARDMAN's (which in turn mirrors the
   ThirdMillennium `auth.Request`/`auth.Response` structs). Where
   CARDMAN's request name is misleading and its stored name is not, the
   stored name is canonical (example: CDRI request `charge_cents` is
   stored as `reported_charge_cents`; canonical is the latter).
2. **One meaning per name.** A name never means two things. Where two
   systems use one name for different things (`call_type`), PIV gives
   one of them a qualified name (`dlog_call_type`) and records the wire
   alias.
3. **Units in the name.** `_cents` (integer, whole cents), `_secs`
   (integer seconds), `_ms` (integer milliseconds), `_ts` (timestamp).
   No floats anywhere. No currency other than the deployment's single
   currency (USD cents today).
4. **Numbers that are identifiers are strings of digits** (`pan`,
   `called_number`, `terminal_id`), never JSON numbers: leading zeros
   matter and some exceed 2^53.
5. **Timestamps**: ISO-8601 UTC with a `Z` suffix. Emitters write
   seconds: `2026-10-13T18:42:55Z`. Readers also accept minute precision
   (`2026-10-13T18:42Z`), because CARDMAN 2.5 serves five fields that way;
   a minute-precision value is never used to order events.
6. **Enumerations are closed.** An unknown value is an error on the card
   plane and on config writes. Event consumers may ignore unknown event
   types but must not guess the meaning of an unknown enum value.
7. **Strictness by plane.** Card plane: strict (unknown or duplicate
   keys fail closed, per `BRIDGE_CONTRACT.md` §2). Config writes: strict.
   Events and config reads: tolerant reader (ignore unknown fields), so
   additive changes do not break subscribers.
8. **Strings sent to CARDMAN** contain no `"` or `\` (CARDMAN's JSON
   reader does not unescape) and ids stay within `[A-Za-z0-9:_-]`.
9. **snake_case**, ASCII, no abbreviations beyond the established ones
   (`pan`, `pin`, `ani`, `dnis`, `npa`, `nxx`, `dlog`).

## 2. Types

| Type | Form | Example |
|---|---|---|
| `digits(n)` / `digits(a..b)` | string of ASCII digits, exact length or range | `"9149000000000011"` |
| `nanp10` | `digits(10)`, a North American number without the leading 1 | `"2125551000"` |
| `ts` | rule 5 | `"2026-10-13T18:42:55Z"` |
| `cents` | non-negative integer | `175` |
| `secs` | non-negative integer | `95` |
| `id` | string, 1-64 chars, `[A-Za-z0-9:_-]` | `"mm-2125551000-17-2026-10-13T18:43:00Z"` |
| `enum(...)` | one of the listed strings | `"approved"` |
| `sha256` | 64 lowercase hex chars | |
| `masked_pan` | first 6 digits, `****`, last 4 (CARDMAN principle 4) | `"914900****0011"` |

## 3. Sensitivity classes

Every field has a class. The class decides where it may travel.

| Class | Meaning | May appear in |
|---|---|---|
| **S3** secret card data | `pan`, `pin`, raw track/stripe data | card plane only: mm_manager memory -> loopback -> bridge -> CARDMAN. Never in logs, SQLite, events, config plane, exports. |
| **S2** linkable or personal | `masked_pan`, `auth_code`, full `called_number`, `dnis`, `dial_id` | card plane, CARDMAN, masked local logs. In events, `called_number` only as `called_number_masked`; `masked_pan` and `auth_code` only to subscribers holding the `events.sensitive` grant. Other subscribers correlate a card by `card_ref` (S1). |
| **S1** operational | terminal ids and line numbers, states, durations, counts, hashes, versions | logs, events, config plane |
| **S0** public | advert text, published prices | anywhere |

`called_number` is S2 because the numbers callers dial are personal
data, not because it is card data.

## 4. Data dictionary

Columns: canonical name; type; class; CARDMAN WAPI name (route); DLOG
source (`src/mm_manager.h` struct.field); mm_manager <-> bridge wire
name (`BRIDGE_CONTRACT.md` §2). A dash means "not present there".

### 4.1 Card instrument

| Canonical | Type | Class | CARDMAN | DLOG | Bridge hop | Notes |
|---|---|---|---|---|---|---|
| `pan` | `digits(16)` | S3 | `pan` (CAUT, CDRI) | `dlog_mt_funf_card_auth.card_number`, `dlog_mt_call_details.card_num` (packed BCD) | `pan` | 16 digits for CARDMAN cards (ADR 10). |
| `masked_pan` | `masked_pan` | S2 | `pan_masked` (CINQ) | - | - | Only PAN form allowed outside the card plane. Nearly unique within a telco (last 4 = 3 `dial_id` digits + check digit), so it is a cardholder tracking key: `events.sensitive` only. |
| `card_ref` | 16 lowercase hex | S1 | - | - | - | HMAC-SHA256 of `pan` under a key held only by the bridge, truncated. Same card -> same `card_ref`; not reversible without the key. The default card identifier in events. |
| `dial_id` | `digits(7)` | S2 | `dial_id` (CDRI alt, CINQ masked) | - | - | Not used by payphones; listed for AGI interchange. |
| `pin` | `digits(4)` | S3 | `pin` (CAUT funf) | `dlog_mt_funf_card_auth.pin` (4 BCD) | `pin` | 4 digits only on this wire (D4). |
| `service_code` | `digits(3)` | S1 | `service_code` | `dlog_mt_funf_card_auth.service_code` | `service_code` | |
| `card_expiry` | `YYYY-MM` | S2 | `expiry` (`YYYY-MM-DD`, issuance) | `exp_yy`, `exp_mm` | - | Not forwarded today. |

### 4.2 Authorization

| Canonical | Type | Class | CARDMAN | DLOG | Bridge hop | Notes |
|---|---|---|---|---|---|---|
| `variant` | `enum(simple, funf)` | S1 | `variant` | - | - (bridge sets `funf`) | |
| `seq_num` | integer 0-65535 | S1 | `seq_num` | `dlog_mt_funf_card_auth.seq` | `seq_num` | Wraps. Part of CAUT idempotency. |
| `requested_at` | `ts` | S1 | `requested_at` | - (manager clock) | `requested_at` | Byte-identical on retry, or a second hold is placed. |
| `called_number` | `digits(1..20)` | S2 | `called_number` (normalized) | `dlog_mt_funf_card_auth.phone_number` (auth), `called_num` (CDR) | `called_number` (as dialed) | Bridge normalizes (`BRIDGE_CONTRACT.md` §3). **Trap**: the DLOG auth field named `phone_number` is the *dialed* number, not the terminal's line. |
| `called_number_masked` | string | S1 | - | - | - | first 6 digits + `****`; event form. |
| `international` | `enum(yes, no)` | S1 | `international` (also accepts `unknown`) | derived: `call_type & 0x0F == 0x06` | `international` | PIV never emits `unknown`. |
| `disposition` | `enum` §6.1 | S1 | `disposition` | - | `disposition` | |
| `terminal_verdict` | `enum(valid, invalid)` | S1 | - | `dlog_mt_auth_resp_code.resp_code` (0 / non-0) | `approved` (bool) | Only `approved` -> `valid`. |
| `auth_code` | string, 8 chars `[0-9A-Z]` | S2 | `auth_code` = `transaction_ref` | `auth_code` (`uint64`, base-36 value) | `auth_code` | Base-36, not hex (R3). |

### 4.3 Terminal

| Canonical | Type | Class | CARDMAN | DLOG | Bridge hop | Notes |
|---|---|---|---|---|---|---|
| `terminal_id` | `nanp10` | S1 | `terminal.terminal_id` (CAUT), `switch_id` (CDRI) | terminal id from the session | `terminal_id` | mm_manager identifies a terminal by its own line number. A claim, not authenticated. Configuration therefore follows the line: a payphone moved to another line is a new `terminal_id`. |
| `terminal_phone_number` | `nanp10` | S1 | `terminal.phone_number` (CAUT), `ani` (CDRI) | session terminal id | `phone_number` | The payphone's own line. Not named `phone_number` because DLOG uses that name for the dialed number. |
| `telco_id` | string | S1 | `terminal.telco_id`, `telco_id` | - | - | CARDMAN sub-tenant. |
| `dlog_telco_id` | integer 0-255 | S1 | - | `dlog_mt_rate_table.telco_id` | - | Terminal-side telco byte. Not the same thing as `telco_id`. |
| `terminal_serial` | string | S1 | `terminal.terminal_serial` | - | - | Sent empty today. |
| `terminal_model` | `enum(coin_basic, card, desk, inmate, multipay, ...)` | S1 | - | `term_type_to_model()` | - | Selects table profile directory. |
| `mtr_version` | string | S1 | - | `term_type_to_mtr()` (`MTR_2_X`, `MTR_1_20`, ...) | - | Selects the table list. |

### 4.4 Call record

| Canonical | Type | Class | CARDMAN | DLOG | Bridge hop | Notes |
|---|---|---|---|---|---|---|
| `cdr_id` | `id` | S1 | `cdr_id` | - | `cdr_id` | `mm-<terminal>-<seq>-<start_ts>`; unique forever; byte-identical on resend. |
| `record_seq` | integer 0-65535 | S1 | - | `dlog_mt_call_details.seq` | `seq` | Not the same counter as `seq_num`. |
| `start_ts` | `ts` | S1 | `start_ts` | `start_timestamp` (terminal local time, converted to UTC) | `start_ts` | |
| `answer_ts` | `ts` | S1 | `answer_ts` | - | - | mm_manager has none; omitted. |
| `duration_secs` | `secs` | S1 | `duration_secs` | `call_duration` | `duration_secs` | Bridge sends 0 for unanswered/abandoned/denied. |
| `dlog_call_type` | integer 0-255 | S1 | - | `call_type` (CALLTYP; low nibble §6.3, bit 7 `FLAG_CDR_IXL`) | `call_type` | Wire alias `call_type` on the bridge hop only. |
| `call_type` | `enum(DOM, INTL, TOLLFREE)` | S1 | `call_type` (rates, CDR rating) | - | - | CARDMAN tariff class. Derived by CARDMAN from `called_number`. |
| `dlog_cdr_rate_type` | integer 0-255 | S1 | - | `dlog_mt_call_details.rate_type` | `rate_type` | Carried verbatim; meaning on the CDR unverified. Not the RATE entry's `dlog_rate_type`. |
| `cost_collected_cents` | `cents` | S1 | - | `call_cost[0]` | `call_cost_collected` | Terminal-reported. |
| `cost_requested_cents` | `cents` | S1 | - | `call_cost[1]` | `call_cost_requested` | Terminal-reported. |
| `reported_charge_cents` | `cents` | S1 | request `charge_cents`, stored `reported_charge_cents` (CDRI) | - | - | `cost_requested_cents`, else `cost_collected_cents`, else 0. Not billed (ADR 8). |
| `charged_cents` | `cents` | S1 | `charged_cents` (CDRI response) | - | - | What CARDMAN actually collected. |
| `cdr_state` | `enum` §6.2 | S1 | `state` | - | `state` | |
| `recorded_state` | `enum` §6.2 | S1 | `recorded_state` | - | - | Differs from `cdr_state` only on `DUPLICATE`. |
| `delivered` | boolean | S1 | - | - | `delivered` | Bridge's verdict to mm_manager. |
| `ani` | `nanp10` | S1 | `ani` | - | - | = `terminal_phone_number` for payphones. |
| `dnis` | `digits(1..20)` | S2 | `dnis` | - | - | = normalized `called_number`. |
| `source` | string | S1 | `source` | - | - | `mm_manager` for this front end. |

### 4.5 Tariff (CARDMAN)

| Canonical | Type | Class | CARDMAN (`rates` table / `GET /api/rates`) | Notes |
|---|---|---|---|---|
| `telco_id`, `call_type`, `carrier` | string, enum, string | S0 | same | `*` is a wildcard. |
| `distance_band` | string or `*` | S0 | column only, not in `GET /api/rates` | `LOCAL`, `INTRALATA`, ... |
| `rate_period` | string or `*` | S0 | column only | `DAY`, `EVENING`, ... |
| `initial_cents`, `initial_secs`, `second_cents`, `second_secs`, `addl_cents`, `addl_secs` | `cents`, `secs` | S0 | same | Three-block tariff. |
| `connection_fee_cents`, `minimum_charge_cents`, `surcharge_cents` | `cents` | S0 | column only | |
| `tariff_version` | integer | S0 | `version` | Orders edits of one row. |
| `effective_ts` | `ts` (minute precision on 2.5) | S0 | `effective_ts` | Last-modified, not a future date. |

### 4.6 Terminal configuration (this fork)

| Canonical | Type | Class | DLOG | Notes |
|---|---|---|---|---|
| `advert_set_id` | `id` | S1 | - | |
| `slot` | integer 1-20 | S0 | index into `dlog_mt_advert_prompts.entry` | 1-10 on hook, 11-20 off hook. |
| `hook_state` | `enum(on_hook, off_hook)` | S0 | derived from `slot` | Read-only. |
| `text` | string, 0-20 printable ASCII | S0 | `message_text` | Never truncated. |
| `display_time_ms` | integer 0 or 10-655350, step 10 | S0 | `display_time` (10 ms units) | |
| `effect` | `enum` §6.4 | S0 | `display_attr` low bits | |
| `blank_before` | boolean | S0 | `display_attr & 0x40` | |
| `rate_plan_id` | `id` | S1 | - | |
| `index` | integer 0-127 | S0 | RATE entry index | |
| `dlog_rate_type` | `enum` §6.5 | S0 | `rate_table_entry.type` | |
| `initial_period_secs`, `additional_period_secs` | integer 1-32767, or `"unlimited"` | S0 | `initial_period`, `additional_period` (bit 15 = unlimited) | |
| `initial_charge_cents`, `additional_charge_cents` | integer 0-65535 | S0 | `initial_charge`, `additional_charge` | |
| `country_code`, `rate_index` | integer; 28-91 | S0 | `intl_rate_table_entry.ccode`, `flags + 28` | |
| `tolerance` | `enum(exact, approximate)` | S0 | - | Imported-plan honesty label. |
| `lossy` | boolean | S0 | - | Entry could not represent the source exactly. |
| `table_id` | string `0x..` | S1 | DLOG table id | Hex string, e.g. `"0x1d"`. |
| `content_sha256` | `sha256` | S1 | - | Of the compiled table file. |
| `deployment_state` | `enum(staging, staged, partially_delivered, delivered, failed, superseded)` | S1 | - | |
| `table_state` | `enum(staged, delivered, drift, failed)` | S1 | - | `delivered` only with a receipt. |

## 5. Event envelope

CloudEvents 1.0, JSON structured mode. Required attributes are
CloudEvents'; `pivversion` is a PIV extension attribute.

```json
{
  "specversion": "1.0",
  "id": "01JABCDXYZ...",
  "source": "urn:piv:mm-bridge:mm-prod",
  "type": "io.github.onthestrugglebus.piv.call.recorded.v1",
  "time": "2026-10-13T18:44:41Z",
  "subject": "terminal/2125551000",
  "datacontenttype": "application/json",
  "pivversion": "1",
  "data": {
    "cdr_id": "mm-2125551000-17-2026-10-13T18:43:00Z",
    "terminal_id": "2125551000",
    "card_ref": "3f9a1c07d2b84e61",
    "called_number_masked": "212555****",
    "start_ts": "2026-10-13T18:43:00Z",
    "duration_secs": 95,
    "reported_charge_cents": 175,
    "cdr_state": "MATCHED",
    "charged_cents": 175
  }
}
```

- `id`: unique per event, used to dedupe (at-least-once delivery).
- `source`: `urn:piv:<component>:<deployment>`.
- `subject`: `terminal/<terminal_id>`, `advert-set/<id>`,
  `rate-plan/<id>`, or `deployment/<id>`.
- `data` holds only S0-S1 fields, plus `called_number_masked`.
  `masked_pan` and `auth_code` are added only for `events.sensitive`
  subscribers (§3). Never S3.
- Events are informational, not billing records. CARDMAN is the only
  money record; consumers must dedupe on `id` before summing anything.

### 5.1 Event types (v1)

| Type | Emitted by | When | Key `data` fields |
|---|---|---|---|
| `io.github.onthestrugglebus.piv.card.authorization.decided.v1` | bridge | after a CAUT exchange | `terminal_id`, `card_ref`, `seq_num`, `disposition`, `terminal_verdict`, `called_number_masked`, `international` |
| `io.github.onthestrugglebus.piv.card.hold.released.v1` | bridge | release CDR sent for `approved_limited` or a lost approval | `terminal_id`, `card_ref`, `cdr_id`, `cdr_state` |
| `io.github.onthestrugglebus.piv.call.recorded.v1` | bridge | after a CDRI exchange | §4.4 fields, masked |
| `io.github.onthestrugglebus.piv.call.delivery_failed.v1` | bridge | CDR not accepted (`delivered:false`) | `cdr_id`, `terminal_id`, `retryable` |
| `io.github.onthestrugglebus.piv.terminal.status.v1` | termcfg | new status/version rows seen | `terminal_id`, `terminal_model`, `mtr_version`, last call-in |
| `io.github.onthestrugglebus.piv.config.advert_set.approved.v1` | termcfg | approval | `advert_set_id`, `version`, `content_sha256`, `self_approved` |
| `io.github.onthestrugglebus.piv.config.rate_plan.approved.v1` | termcfg | approval | `rate_plan_id`, `version`, `tolerance`, `self_approved` |
| `io.github.onthestrugglebus.piv.config.rate_plan.source_failed.v1` | termcfg | an import failed a fail-safe | `rate_plan_id`, `source.kind`, `error` |
| `io.github.onthestrugglebus.piv.config.table.delivered.v1` | termcfg | receipt matches staged hash | `terminal_id`, `table_id`, `content_sha256`, `deployment_id` |
| `io.github.onthestrugglebus.piv.config.table.drift.v1` | termcfg | managed file hash unknown | `terminal_id`, `table_id`, `content_sha256` |

## 6. Enumerations

### 6.1 `disposition` (CARDMAN CAUT) -> `terminal_verdict`

| `disposition` | `terminal_verdict` | Note |
|---|---|---|
| `approved` | `valid` | |
| `approved_limited` | `invalid` | and a release CDR (ADR 40) |
| `requires_card`, `declined`, `bad_card`, `bad_country`, `no_tariff` | `invalid` | |
| (non-200, timeout, empty or unparseable body) | `invalid` | PIV disposition `transport_error`, bridge-local, never sent to CARDMAN |

### 6.2 `cdr_state` (CARDMAN CDRI)

`MATCHED`, `LATE`, `UNMATCHED`, `DUPLICATE`, `MERGED`, `FILTERED`,
`NOTARIFF`. Delivery verdict: any of these -> `delivered:true`;
transport error, `INTERNAL`, HTTP 400 -> `delivered:false`.

### 6.3 `dlog_call_type` low nibble (CALLTYP)

| Value | Name | Value | Name |
|---|---|---|---|
| `0x0` | `incoming` | `0x8` | `zero_plus` |
| `0x1` | `unanswered` | `0x9` | `toll_free_800` |
| `0x2` | `abandoned` | `0xa` | `directory_assistance` |
| `0x3` | `local` | `0xb` | `denied` |
| `0x4` | `intra_lata` | `0xc`, `0xd` | `unassigned` |
| `0x5` | `inter_lata` | `0xe` | `e_purse` |
| `0x6` | `international` | `0xf` | `unknown` |
| `0x7` | `operator` | | |

Bit 7 (`FLAG_CDR_IXL`) marks an international CDR. Source:
`CALL_TYPE_*` in `src/mm_manager.h`.

### 6.4 `effect` (ADMESS `display_attr`)

| `effect` | Code | Verified meaning? |
|---|---|---|
| `permanent` | 0 | from upstream constants |
| `flash_same` | 1 | " |
| `flash_next` | 2 | " |
| `scroll_in` | 3 | " |
| `scroll_out` | 4 | " |
| `scroll_in_out` | 5 | " |
| `new_prompt` | 17 | name only (H-W2) |
| `flash_same_off` | 18 | name only |
| `flash_next_2nd` | 19 | name only |
| `flash_next_1st_off` | 20 | name only |
| `flash_next_2nd_off` | 21 | name only |

`blank_before` ORs `0x40` into the code.

### 6.5 `dlog_rate_type` (RATE entry, `rate_type_t`)

`mm_intra_lata` (0), `lms_rate_local` (1), `fixed_charge_local` (2),
`not_available` (3, not valid in a RATE entry), `invalid_npa_nxx` (4),
`toll_intra_lata` (5), `toll_inter_lata` (6), `mm_inter_lata` (7),
`mm_local` (8), `mm_international` (9).

## 7. Error envelope

Same shape as CARDMAN's, so one parser handles both:

```json
{"ok": false, "error": "human-readable reason", "code": "VALIDATION_FAILED",
 "details": [{"field": "slots[3].text", "rule": "max_length_20"}]}
```

`code` values: CARDMAN's (`BAD_REQUEST`, `FORBIDDEN`, `CONFLICT`,
`RATE_LIMITED`, `INTERNAL`, `NO_TARIFF`, `CARD_NOT_FOUND`, ...) plus the
config plane's `VALIDATION_FAILED`, `PRECONDITION_FAILED` (stale
`If-Match`), `APPROVAL_REQUIRED`, `AMBIGUOUS_SOURCE`, `STALE_SOURCE`,
`NOT_FOUND`. `details` is optional and never echoes a submitted value
that could be S2/S3.

HTTP status follows CARDMAN's rule: 200 for a well-formed business
outcome, 4xx for a malformed or refused request, 5xx for a server fault.

## 8. Versioning

- `pivversion` / `piv_version` is the major version, a string.
- Adding an optional field, an event type, or an enum value to an
  **event** is a minor change (tolerant readers). Adding an enum value to
  a card-plane or config-write field is a major change (strict readers
  would fail closed on it).
- Renaming or removing a field, or changing a unit or a type, is a major
  change. Two majors may be served side by side for one release.
- Each release of this fork records which CARDMAN commit its mappings
  were checked against (as `BRIDGE_CONTRACT.md` does today) and the
  contract tests in `AI/TASKS.md` T15 are generated from §4.

## 9. Mapping other platforms (examples)

| Platform | How it maps |
|---|---|
| Asterisk AGI (CARDMAN dial-in) | Already speaks CARDMAN names (`wapi.py`): `pan` or `dial_id`, `auth_code`, `cdr_id`, `ani`, `dnis`. Its CDRs map to `io.github.onthestrugglebus.piv.call.recorded.v1` with `source:"realtime"`. |
| Tranz gateway (CARDMAN ADR 11, spec only) | `terminal_id` and `operator_ref` are opaque data fields there, as here; map `terminal_id` directly. |
| Generic webhook consumer / BI tool | Subscribe to events; everything arrives masked and typed per §4. |
| Spreadsheet export of adverts or rate plans | One row per slot / entry, columns = §4.6 canonical names. The CSV layout of `config/advert_prompts.csv` maps to `display_time_ms`, raw code, `text`. |

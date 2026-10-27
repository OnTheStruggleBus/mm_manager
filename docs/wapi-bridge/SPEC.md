# WAPI bridge architecture spec

Status: **Proposed** (2026-10-18). Spec only; nothing here is built.
Decision record: [`ADR-0001-WAPI-BRIDGE.md`](ADR-0001-WAPI-BRIDGE.md).
Field names and envelopes: [`VOCABULARY.md`](VOCABULARY.md).
Red-team pass over this document: [`ADVERSARIAL_REVIEW.md`](ADVERSARIAL_REVIEW.md).

Sources read for this spec (read-only, never edited from here):

- This fork at `main` `c8c5c11`: `src/mm_manager.c`, `src/mm_manager.h`,
  `src/mm_cardman*.c`, `tools/cardman_bridge.py`,
  `generate_advert_prompts.py`, `src/mm_admess.c`, `src/mm_rate.c`,
  `docs/cardman-compat/*`, `AI/*`.
- CARDMAN (`OnTheStruggleBus/MCardSys`) at `origin/main` `9997d87`:
  `docs/api/API_CONTRACTS.md`, `docs/frontends/MILLENNIUM_GATEWAY_CONTRACT.md`,
  `docs/ai/ARCHITECTURE_DECISIONS.md` (ADR 4, 8, 11, 13, 14, 15, 18, 26,
  31, 38, 39, 40, 41, 42), `docs/architecture/BRICKS_REFERENCE.md`,
  `docs/ai/KNOWN_BRICKS_QUIRKS.md`, `docs/ai/CARDMAN_2.5.1_TODO.md`,
  `bricks/runtime/web_routes.conf`, `bricks/sql/schema.sql` (rates).

Where this spec and a CARDMAN contract disagree, the CARDMAN contract
wins (CARDMAN `AGENTS.md` "docs win"; fork `BRIDGE_CONTRACT.md` header).
Where this spec and `AI/SECURITY_RULES.md` disagree, the security rules
win.

---

## 1. Scope

**In scope**

1. The production **WAPI bridge**: the process that turns mm_manager's
   card authorizations and call records into CARDMAN WAPI calls (`CAUT`,
   `CDRI`). Its hop contract already exists and is live-proven
   (`docs/cardman-compat/BRIDGE_CONTRACT.md`); this spec adds the
   production shape around it.
2. A **terminal configuration API** for things a payphone displays and
   charges by locally: advertising prompts (DLOG table `0x1d`) and rate
   tables (`0x49` RATE, `0x97` INTL SBR, plus the NPA-NXX/LCD tables that
   point into RATE).
3. A **standard vocabulary** for every piece of call, card, terminal and
   configuration information, usable on every hop and by other apps
   ([`VOCABULARY.md`](VOCABULARY.md)).
4. The CARDMAN/BRICKS-side constraints and the requests this design
   makes of CARDMAN.

**Out of scope**

- Any change to CARDMAN money behaviour (hold sizing, rating, capture).
  Those stay CARDMAN's (gateway contract §1).
- Smartcard / Mondex (CARDMAN-side deferral, `AI/CARDMAN_CONTEXT.md`).
- Changing the DLOG protocol or terminal firmware.
- Code. Every component below is a design.

## 2. Context

```
                         Nortel Millennium terminal
                                   | 1200 baud dial-in (terminal always calls)
                                   v
 +-------------------------- payphone host ---------------------------------+
 |                                                                          |
 |  mm_manager (fork, one process per modem line)                           |
 |    DLOG protocol, table download, masked SQLite                           |
 |      |  card plane: loopback HTTP/JSON          ^ reads staged tables     |
 |      |  /authorize, /cdr (PAN, PIN)             | (files, no network)     |
 |      v                                          |                         |
 |  mm-bridge  (card plane)                    table store                   |
 |    payphone identity, mTLS                  tables/<tid>/, tables/default |
 |    egress: CARDMAN only                         ^                         |
 |      |                                          | atomic writes           |
 |      |  masked events (local spool)             |                         |
 |      +----------------------------->  mm-termcfg  (config plane)          |
 |                                         adverts, rate plans, deployments |
 |                                         config API (own identities)      |
 |                                         event relay (masked only)        |
 +------|-------------------------------------------|--------------|--------+
        | mTLS + Basic (payphone, CN mm-<env>)      | rate source  | webhooks /
        v                                           v (DW-2)       v config API
     CARDMAN (BRICKS) TERMREG WAPI           CARDMAN GET /api/rates   other apps
        CAUT, CDRI                           or operator file, or
                                             2.6 QUOT
```

Two planes, kept apart on purpose (ADR decision W1):

| | Card plane (`mm-bridge`) | Config plane (`mm-termcfg`) |
|---|---|---|
| Carries | PAN, PIN, auth_code, call records | advert text, rate plans, table files, masked events |
| Callers | mm_manager only, loopback | operators, tools, other apps (per-client identities) |
| Egress | CARDMAN `CAUT`/`CDRI` only | CARDMAN rate source (DW-2), event webhooks |
| Availability | in the call path; a failure declines cards | off the call path; a failure delays config changes |
| Security rule it lives under | `SECURITY_RULES.md` 1-9, fully | no card data may enter it, ever |

## 3. Components

### 3.1 mm_manager (fork)

Unchanged role. Fork-delta additions this design needs (all at call
sites, new logic in new `mm_termcfg*.c` files per `AI/REPO_GUIDE.md`
"Fork delta"):

| # | Change | Why | Verified? |
|---|---|---|---|
| F1 | Write a **delivery receipt** when a table sent by `mm_download_tables()` is ACKed, that is when `wait_for_table_ack()` (`src/mm_proto.c`) returns 0 for it: `(terminal_id, table_id, sha256 of the file bytes as loaded, before any in-memory patch, the path the file was resolved from, ack time UTC)`, appended to `tables/<tid>/delivery.log` (one JSON object per line). A failed ACK writes a `failed` line, not a receipt. At session end, also write `tables/<tid>/terminal.json` (terminal model, MTR version, last session time UTC, no card data). | The config plane must report `delivered`, not guess it. During a manager-pushed download the ACKs are consumed by `wait_for_table_ack()`, whose return value `mm_download_tables()` ignores; the `case DLOG_MT_TABLE_UPD_ACK` in `process_mm_table()` only sees ACKs outside a download, so a hook there would never fire (independent review I-1). The hash is of the file, not of the sent buffer, because mm_manager patches some buffers after loading (`DLOG_MT_FCONFIG_OPTS` `term_type`, `DLOG_MT_CALL_SCREEN_LIST` padding). `terminal.json` gives `mm-termcfg` its inventory without opening mm_manager's database, which holds masked PANs (review A-18). | Design only. |
| F2 | On `DLOG_MT_CALL_IN`, if `tables/<tid>/pending` exists, send **only** the managed tables whose staged sha256 differs from the last F1 receipt for that table, then `END_DATA` (the upstream download lines are present but commented out in `case DLOG_MT_CALL_IN`). Pending is decided by hash, never by mtime. Behind a run-time flag, default off. | A routine scheduled call-in does **not** download tables today. Only `DLOG_MT_ATN_REQ_TAB_UPD` (craft force, install, lost memory, power lost on download) does. Without F2, a staged change waits for a technician. Sending only changed tables keeps the payphone's line busy for less time (A-17); hashes survive host clock steps that break `check_mm_table_is_newer()`'s mtime compare (A-16). | **Unverified on hardware (H-W1).** Upstream commented it out; the reason is unknown. |
| F3 | Answer `DLOG_MT_RATE_REQUEST` (`0x3f`) from a compiled local lookup file written by `mm-termcfg`, instead of the hard-coded 240 s / 100 c / 60 s / 25 c. One lookup file per published plan version, named by the hash of the RATE table it was compiled with; mm_manager uses the one whose hash matches this terminal's last F1 receipt for `0x49`, never simply the newest. File read only; no network in the protocol loop. No receipt, or a missing or invalid file -> today's behaviour plus a warning. | Otherwise the terminal is told a price that contradicts the RATE table it actually holds. A plan is staged long before it is delivered (until H-W1, until a technician visits), so answering from the newest plan would quote new prices on a terminal still running old ones (independent review I-7). | Design only; which calls trigger `0x3f` is unverified (H-W4). |
| F4 | Accept `-t`/`-d` pointing at the shared table store for every line process (already supported) and document it as required. | One writer (`mm-termcfg`), many readers (one mm_manager per line, `ARCHITECTURE_ANALYSIS.md` G12). | Existing behaviour. |
| F5 | Hold an exclusive `flock` on `tables/<tid>/.lock` and a shared `flock` on `tables/.lock` for the whole of any table download. Take them **before** the reply that announces the download (`DLOG_MT_TABLE_UPD` is sent by `process_mm_table()` before `mm_download_tables()` runs). Wait at most 5 s; if not acquired, skip the download this session and log it, except when the terminal's reason includes lost memory or power lost on download, where it waits up to 60 s (a terminal without tables is out of service). Once a managed rate table (`0x49`, `0x96`, `0x97`, LCD/NPA) fails its ACK, send no further managed rate tables in that session. | Without the lock, a deployment staged mid-download can send old RATE with new NPA-NXX tables (RATE is sent first), pointing callers at the wrong prices (review A-10). Today a failed ACK does not stop the loop, so a NACKed RATE can be followed by ACKed prefix tables (independent review I-2). | Design only; how the terminal handles a download cut short is H-W5. |

The card path (`src/mm_cardman.c`, `src/mm_cardman_bridge.c`) is not
changed by this spec.

### 3.2 mm-bridge (card plane)

The production version of `tools/cardman_bridge.py`. Its contract with
mm_manager is `BRIDGE_CONTRACT.md` §2 and with CARDMAN §3; the port
checklist is `AI/TASKS.md` T16. This spec adds:

1. **Listener**: loopback TCP or a Unix socket (mode 0600, owned by the
   service user both processes run as). No other listener. No config
   API, no admin UI, no metrics port on this process.
2. **Egress allow-list**: the CARDMAN TERMREG WAPI address only (CARDMAN
   ADR 13 decision 3: never ADMINREG/FRAUDREG ports). Enforced in the
   process (refuse any other host) and, where the host allows it, by a
   firewall rule.
3. **Health**: `GET /healthz` on the same loopback listener, returning
   `{"ok":true,"piv_version":"1","cardman_reachable":<bool>,"last_caut_ok_ts":"...","last_cdri_ok_ts":"..."}`.
   No card data, no counts by card. `cardman_reachable` comes from the
   last real call's outcome; the bridge does not probe CARDMAN with
   synthetic money calls. If that outcome is older than 15 minutes,
   `cardman_reachable` is `null` (unknown), not the stale value
   (independent review I-21).
4. **Masked event spool**: after each CAUT/CDRI exchange, append one
   event ([`VOCABULARY.md`](VOCABULARY.md) §5) to a local spool directory
   with no S3 field (no PAN, no PIN, no `dial_id`) and S2 fields only in
   masked form. The card is identified by `card_ref` (HMAC-SHA256 of the
   PAN under a key held only in the bridge's memory, truncated to 16
   hex). The key is random at start and rotated every 24 h; it is never
   written to disk, because anyone holding it can recover a PAN from a
   `card_ref` by enumerating the roughly 10^7 PANs of one telco.
   `card_ref` is S2, like `masked_pan`: a stable per-card pseudonym with
   terminal and time is the same cardholder trail A-3 was about
   (independent review I-4). The relay passes it only to
   `events.sensitive` subscribers, and re-keys it per subscription
   (`HMAC(subscription_secret, card_ref)`), so two subscribers cannot join
   their data on it.
   Spool directory mode 0750, files 0640, owner the bridge user, group
   the relay; every event is checked against the vocabulary schema
   before it is written. The bridge never sends events anywhere itself;
   `mm-termcfg`'s relay reads the spool. This keeps the bridge's only
   egress CARDMAN (rule 5). A full spool (size cap reached) drops events
   and counts the drop; it never blocks or fails a card call. The spool
   is in scope of the `AI/WORKFLOW.md` step 3 decode-and-search leak
   check.
5. **Version marker**: requests and responses on the mm_manager hop may
   carry `"piv_version":"1"`. Absent means version 1. Any other value is
   a failure (fail closed), per the hop's strict-parse rule.
6. **Secrets**: payphone cert, key and Basic-Auth password from files
   outside the repo with mode 0600 (rule 8; CARDMAN ADR 17/42 custody
   pattern). Never in environment dumps or logs.
7. **Terminal allow-list**: the bridge declines (`invalid`, no CARDMAN
   call) any `/authorize` whose `terminal_id` is not on its allow-list.
   The list is a file written by `mm-termcfg` from its inventory and
   read-only to the bridge; a missing or unreadable list declines
   everything (fail closed). A terminal gets on the list only by an
   explicit enrollment (`PUT /v1/terminals/{terminal_id}`, §8.2), never
   automatically from a call-in or `terminal.json`, so an emulator cannot
   enroll itself. `terminal_id` is still only a claim; the list stops
   casual emulators, not one who knows a real line number.
8. **Attempt limits** (defaults; configurable). *Why*: `payphone` is
   exempt from CARDMAN rate limiting (ADR 18) and telco-global (ADR 38),
   and three consecutive wrong PINs lock a card (`pin_fails`), so a DLOG
   emulator dialing in could lock customers' cards at line rate (review
   A-1). Three limits, each answering `invalid` locally without calling
   CARDMAN:
   - **Per card**: after 2 consecutive `invalid` verdicts for one card
     (keyed by the in-memory card HMAC), further attempts for that card
     are refused for 24 h or until the bridge restarts. This keeps the
     payphone path from delivering a card's third strike by itself, which
     the per-terminal and fleet limits cannot do: at 60 per 10 minutes
     they still let an attacker lock 20 cards every 10 minutes
     (independent review I-6). A legitimate caller who mistypes twice
     waits; that is cheaper than a locked card.
   - **Per terminal**: more than 5 `invalid` in 10 minutes refuses that
     `terminal_id` until the window passes.
   - **Fleet**: more than 60 `invalid` in 10 minutes raises an alert
     event and drops the per-terminal limit to 1 for the next hour. It
     never refuses the whole fleet: terminal ids are printed on the
     payphones, so a fleet-wide refusal would let anyone stop every
     card call with 61 attempts.
   A limit refuses a spoofed `terminal_id` and the real terminal alike;
   the per-terminal limit is therefore also a way to deny one payphone's
   card service. H-W6 checks whether mm_manager can read the calling
   line's caller ID from the modem and refuse sessions whose claimed
   `terminal_id` disagrees. Caller ID can be forged on some networks, but
   it is harder to forge than a field the emulator fills in itself. A tripped limit raises an event and is
   visible in `/healthz` as a count, never with card data. CDRs are
   never limited.

Everything else (dispositions, release CDRs, number normalization,
retries, `requested_at` cache, timeouts) is exactly T16 / `BRIDGE_CONTRACT.md` §3.

### 3.3 mm-termcfg (config plane)

A new, separate service. Owns:

- **Advert sets** (§5): the 20 display prompts, validated and versioned.
- **Rate plans** (§6): the terminal price list, compiled to DLOG tables.
- **Profiles and terminals**: enrolled terminals (an explicit admin
  action), what each last reported (F1 `terminal.json` and receipts;
  never mm_manager's database), the bridge allow-list derived from
  enrollment, and the target groups configuration is assigned to.
- **Deployments** (§7): staging compiled tables into the table store,
  tracking `staged` -> `delivered` per terminal.
- **Audit log** (§8.4): every change, approval, deployment and import.
- **Event relay** (§9): masked events to subscribed apps.

It never sees card data and never talks to the card plane except by
reading the bridge's masked event spool.

Implementation language: Python stdlib (decision DW-6), because
the table encoders it must reuse, `generate_*.py`,
are Python, and the reference bridge is Python stdlib.

### 3.4 Table store

The directories mm_manager already reads (`load_mm_table()`,
`check_mm_table_is_newer()`):

| Path | Meaning |
|---|---|
| `tables/default/mm_table_XX.bin` | fallback for every terminal (`-d`) |
| `tables/<profile>/mm_table_XX.bin` | model overrides mm_manager already looks for (`coin`, `card_only`, `desk`, `inmate`, `multipay`) |
| `tables/<terminal_id>/mm_table_XX.bin` | per-terminal override (`-t`) |
| `tables/<terminal_id>/table_update.log` | written by mm_manager after a full download (existing) |
| `tables/<terminal_id>/delivery.log` | new, F1 |
| `tables/<terminal_id>/pending` | new, F2 trigger; written by `mm-termcfg`, removed by `mm-termcfg` when every staged table has a matching receipt |
| `tables/<terminal_id>/terminal.json` | new, F1; written by mm_manager at session end |
| `tables/<terminal_id>/.lock`, `tables/.lock` | new, F5; download and staging locks |

`mm-termcfg` is the **only writer** of the **managed** `.bin` files, and
may write no others. Managed tables are exactly: `0x1d` (adverts),
`0x49` (RATE), `0x96`, `0x97`, and the LCD / NPA-NXX ids in §6.1. The
compiler refuses any other table id. Every other table stays owned by
the operator account that installs mm_manager, because several of them
control the card path: the card table `0x16` carries per-range
`vfy_flags` (`src/mm_card.h`) such as `CARD_VF_NCCVAL_IND` (whether the
terminal asks the manager to validate a card at all),
`CARD_VF_PROMPT_FOR_PIN` and `CARD_VF_ACCS_ROUTING`. A config plane that
could rewrite `0x16` could let card calls through with no `CAUT` and no
hold, which W1 says a config bug must never be able to do (independent
review I-3). Enforcement is on disk, not only in code: table
directories are sticky (`chmod +t`), unmanaged files are owned by the
operator account, so `mm-termcfg` cannot rename over them, and the drift
sweep (§7.4) also hashes unmanaged files and raises
`config.table.drift` when one changes.

mm_manager resolves each table per terminal in this order:
`tables/<terminal_id>/`, then the model profile directory under `-t`,
then `-d` (`load_mm_table()`). A file at a more specific level
**shadows** a deployment to a less specific one. Deployments compute the
effective file per terminal with the same order and report a shadowed
table as `shadowed`, naming the file that wins, instead of leaving it
`staged` forever (independent review I-13).

Hand edits and the `generate_*.py` scripts run directly against a
managed store are unsupported once `mm-termcfg` manages it (it detects
them by hash, §7.4).

## 4. Card plane API

No new endpoints beyond `GET /healthz` (§3.2). `POST /authorize` and
`POST /cdr` are `BRIDGE_CONTRACT.md` §2. Their field names map to the
standard vocabulary in [`VOCABULARY.md`](VOCABULARY.md) §4; the wire
names do **not** change (renaming a live, proven contract buys nothing
and risks a field mismatch on the money path).

## 5. Advertising prompts

### 5.1 What the terminal holds

DLOG table `0x1d` `DLOG_MT_ADVERT_PROMPTS` (`src/mm_manager.h`
`dlog_mt_advert_prompts_t`): 20 entries of `admess_table_entry_t`, 24
bytes each; the file is 480 bytes (the table without its id byte).
`generate_advert_prompts.py` does not enforce this: it writes the file
first and only prints a warning if the size is wrong, and it writes text
longer than 20 characters as is, so an oversized table reaches disk
(independent review I-20). `mm_admess` validates the size with
`mm_validate_table_fsize()`. The `mm-termcfg` compiler reuses the
script's byte layout but enforces every size and length itself (§5.2).

| Wire field | Size | Meaning |
|---|---|---|
| `display_time` | `uint16` LE | time to display or scroll, 10 ms units |
| `display_attr` | `uint8` | effect code, plus `0x40` blank-before flag |
| `spare` | `uint8` | upstream comment: "seems to indicate class of number"; always 0 here |
| `message_text` | 20 bytes | text, space padded |

Entries 1-10 are shown on hook (idle), 11-20 off hook
(`generate_advert_prompts.py` header).

### 5.2 Resource: `advert_set`

```json
{
  "advert_set_id": "summer-2026",
  "version": 4,
  "slots": [
    {"slot": 1, "text": "  Local coin calls", "display_time_ms": 6000,
     "effect": "flash_next", "blank_before": false},
    {"slot": 2, "text": "", "display_time_ms": 0, "effect": "permanent",
     "blank_before": false}
  ],
  "content_sha256": "<of the compiled 480-byte table>",
  "state": "approved",
  "authored_by": "ops:alice", "approved_by": "ops:bob"
}
```

Rules (all enforced on write; any failure rejects the whole set with
`VALIDATION_FAILED` and the list of offending slots):

| Field | Rule |
|---|---|
| `slots` | exactly 20 entries, `slot` 1-20, each once. A missing slot is an error, not "blank" (no partial tables). |
| `text` | 0-20 characters, printable ASCII `0x20`-`0x7E` only. Longer text is rejected, never truncated. Padding to 20 is done by the compiler. Non-ASCII is rejected until the terminal VFD character set is verified (H-W2). |
| `display_time_ms` | integer, 0 or 10-655350, multiple of 10. 0 is passed through as 0 (used by the stock CSV; its terminal meaning is unverified, H-W2). |
| `effect` | one of the `VOCABULARY.md` §6.4 names; each maps to one ADMESS code (`ADMESS_ATTR_*` in `src/mm_manager.h`). Unknown names rejected. Codes 17-21 are accepted but flagged "meaning unverified" in previews. |
| `blank_before` | boolean; sets `0x40`. |
| `spare` | not exposed; always 0. |

**Content policy** (decision DW-4: flag, decided 2026-10-18):

- A set is flagged if any of these match, and then needs approval by a
  user holding the `rates.approve` or `adverts.approve` role even in
  single-operator mode, with the reason recorded. A payphone display is
  a trusted surface to callers; a phone number or price on it is the
  shape of a toll-fraud or phishing message.
  - **Number**: 7 or more digits after removing spaces, dashes, dots and
    parentheses, checked on each slot **and** on the concatenation of
    each hook state's slots in display order (1-10, 11-20), so a number
    split across slots is caught. Letters next to digits count as
    keypad digits for this check (`1-800-CALL-NOW`).
  - **Price**: a `$` amount, or digits followed by `c`, `ct`, `cent` or
    `cents` (the stock `config/advert_prompts.csv` says `50-cents` and
    `$1.00 for 4-minutes`), or `free`.
  (independent review I-9)
- Prices in adverts are not cross-checked against the rate plan
  automatically in phase 1; the preview shows the current published rate
  plan next to the advert so the approver can. The reverse also holds: a
  rate plan's approval and deployment preview list every slot flagged
  **Price** in the advert sets in effect on the target terminals
  (including the `-d` default set), and the approval body must
  acknowledge them. A rate change can make an advert already on the
  terminal false, without anyone touching the advert (independent
  review I-8).

### 5.3 Compile and preview

Compile = the byte layout `generate_advert_prompts.py` produces (the
compiler reuses that encoder logic, not a second implementation of it).
Preview returns: the compiled bytes' sha256, a decode of the compiled
bytes by the same dumper logic as `mm_admess` (round-trip proof), and a
plain-text rendering of both hook states.

## 6. Rate tables

### 6.1 What the terminal holds, and why it matters

| Table | Id | Shape (`src/mm_manager.h`) |
|---|---|---|
| RATE | `0x49` | header (timestamp, `telco_id` `uint8`, 32 spare bytes) + 128 entries of `{type, initial_period, initial_charge, additional_period, additional_charge}`; file 1191 bytes. `FLAG_PERIOD_UNLIMITED` is bit 15 of a period. |
| INTL SBR | `0x97` | country code -> flags; flags 2-63 point at RATE entry `flags + 28` (`IXL_TO_RATE`), so RATE entries 30-91; 0 = NCC rated, 1 = blocked. The header's `default_rate_index` uses the same encoding (`src/mm_rateint.c`). |
| NPA-NXX / LCD | `0x4a`-`0x51`, `0x5a`/`0x5b`, `0x65`-`0x73`, `0x88`-`0x95`, `0xa0`/`0xa1`, NPA SBR `0x96` | map dialed prefixes to a rate type / entry; built today by `generate_lcd*.py`, `generate_npa.py`. |

Units, from upstream's non-test `DLOG_MT_RATE_REQUEST` answer
(240 / 100 / 60 / 25 = "$1.00 for 4 minutes, then 25 c a minute"):
periods in seconds, charges in cents.

**The RATE table is terminal-wide and prices coin calls too.** Entries
are keyed by call/rate type (`rate_type_t`: local, intra-LATA,
inter-LATA, international, ...), not by payment method. The terminal
uses the same entries to collect coins and to compute the
`call_cost` it reports on a card CDR. CARDMAN's tariff is a card tariff.
So "load CARDMAN rates into the terminal" also reprices coin calls.
That is decision **DW-1**, decided (b): the terminal table stays the coin price list and CARDMAN rates only feed a drift report until H-W3 says otherwise; this spec does not let a CARDMAN import
change coin pricing without a human approving that exact diff (§6.4).

### 6.2 Resource: `rate_plan`

A rate plan is the terminal price list in the vocabulary's terms
(`VOCABULARY.md` §6.5), independent of where its numbers came from:

```json
{
  "rate_plan_id": "nyc-2026q4",
  "version": 7,
  "dlog_telco_id": 1,
  "entries": [
    {"index": 8, "dlog_rate_type": "mm_local",
     "initial_period_secs": 240, "initial_charge_cents": 100,
     "additional_period_secs": 60, "additional_charge_cents": 25}
  ],
  "intl": {"default": "ncc_rated", "countries": [{"country_code": 44, "rate_index": 30},
                                                 {"country_code": 850, "rate_index": "blocked"}]},
  "prefix_map_ref": "lcd-nyc-2026q4",
  "source": {"kind": "manual|cardman_rates|cardman_quote|operator_file",
             "fetched_ts": "...", "source_digest": "<sha256 of the source list>"},
  "state": "draft|approved|published|retired"
}
```

Validation: `index` 0-127 and unique; periods 1-32767 or the unlimited
flag; charges 0-65535; `dlog_rate_type` from `rate_type_t` (excluding
`not_available`, which upstream marks "not used in RATE table"); INTL
`rate_index` and `default` are each `"ncc_rated"`, `"blocked"`, or a
RATE index 30-91 (flags 2-63 + offset 28). The first draft said 28-91;
28 and 29 would compile to flags 0 and 1, silently turning a priced
country into "NCC rated" or "blocked", and its example's
`default_rate_index: 0` meant NCC rated, not RATE entry 0 (independent
review I-10). Index layout beyond
that (which index each LCD/NPA table points at) must match the
existing generators; the compiler refuses a plan whose prefix map points
at an index the plan does not define.

**Index stability** (independent review I-2). A terminal can end up
holding RATE from one plan version and prefix tables from another even
with the F5 lock: a line drop or a failed ACK part way through a
download leaves whatever was already ACKed, and RATE is sent first.
So a new version may change the **prices** at an index, but may not
change which kind of call an index means while the previous published
version's prefix map points at it. Re-pointing is done in two
deployments: first add the new indexes (old prefix map, new RATE), wait
until every target terminal has a receipt, then switch the prefix map.
The compiler checks this against the version currently published to
each target and refuses otherwise. Any mix of two adjacent versions is
then priced by one version or the other, never by an index that means
something else.

The prefix map (LCD/NPA tables) is a separate versioned artifact built
from the existing generators' inputs (rate centers, NPA lists). Phase 1
treats it as an operator-supplied input and only checks references.

### 6.3 Sources

| Kind | How | When usable | Credential |
|---|---|---|---|
| `manual` | operator enters or uploads entries | now | config-plane role only |
| `operator_file` | an admin runs `GET /api/rates` from their own workstation with their own `admin` identity and uploads the JSON; `mm-termcfg` validates it as if it had fetched it, and records the source as `operator_attested` with the uploader's identity (CARDMAN does not sign responses, so the file's origin cannot be proven; review A-19) | now (CARDMAN 2.5.0.1) | none held by the payphone host |
| `cardman_rates` | `mm-termcfg` pulls `GET /api/rates` on a schedule | only after CARDMAN grants a **read-only** identity (request R9, §10.2) | dedicated `ratesync` identity, never `mediation` on this host |
| `cardman_quote` | `mm-termcfg` samples CARDMAN 2.6 `POST /api/rates/quote` (`QUOT`) over a destination x period x duration matrix | CARDMAN 2.6 (proposed, not built) | `ratesync` only (R10). Never the `payphone` identity, although CARDMAN's proposed `QUOT` ACL is `payphone,mediation,admin`: W1 gives the planes no shared credential, and `payphone` also grants `CAUT` and `CDRI` (independent review I-15). |

Why not `mediation` (the only group that can read `/api/rates` today
besides `admin`): it also grants `CDRI`. A leaked `mediation` credential
on the payphone host can post call records that capture open holds on
any card (`CDRI` keys on `auth_code`, and `UNMATCHED` CDRs are retried by
the `CDRS` sweep). The sync job would hold a money-moving credential to
read a price list. Rejected (ADR W5).

### 6.4 CARDMAN rows -> terminal entries

`GET /api/rates` returns, per row: `telco_id`, `call_type`
(`DOM|INTL|TOLLFREE`), `carrier`, `initial_cents/secs`,
`second_cents/secs`, `addl_cents/secs`, `version`, `effective_ts`. It
does **not** return `distance_band`, `rate_period`,
`connection_fee_cents`, `minimum_charge_cents`, `surcharge_cents` or
per-number overrides (API_CONTRACTS `GET /api/rates`). The terminal entry
has two blocks, CARDMAN rows have three plus fees. So the mapping is
lossy and the plan records how lossy:

1. **Select** rows for the plan's `telco_id` (config: CARDMAN
   `telco_id` -> plan). The response is every telco's rows, with no
   scoping, and includes `*` rows (CAUT's `no_tariff` rule names the
   universal `'*'/'*'/'*'` row). For each `call_type`, use the plan's
   telco's rows if there are any, else the `*` telco's rows; never mix
   the two for one `call_type`. (This is how CAUT's `no_tariff` rule
   reads; the exact tie-break inside CARDMAN's rater is not in the
   contracts, which is one more reason the label stays `approximate`.) Reject the
   import if two rows share `(telco_id, call_type, carrier)` (they
   differ by band or period, which the response cannot show):
   `AMBIGUOUS_SOURCE`, human must pick. If several **carrier** rows
   apply to one `call_type`, the terminal cannot tell carriers apart:
   take the highest price per block across them and flag the entry
   `lossy:true` (independent review I-12).
2. **Map** `call_type` to terminal rate types with an operator-owned
   table (default: `DOM` -> `mm_local`, `mm_intra_lata`,
   `mm_inter_lata`; `INTL` -> international entries; `TOLLFREE` -> none,
   since the terminal does not charge 1-800 calls).
3. **Blocks**: `initial_* -> initial_*`; `addl_* -> additional_*`. If
   `second_*` differs from `addl_*`, the second block cannot be shown.
   Default rule **never understate**: fold the second block into the
   first, `initial_period = initial_secs + second_secs` and
   `initial_charge = initial_cents + second_cents`, and flag the entry
   `lossy:true`. This overstates calls shorter than `initial_secs`. The
   first draft raised `initial_charge` but kept `initial_period =
   initial_secs`, which bills the second block's time at the `addl`
   rate and so understates whenever `second` is dearer than `addl`
   (independent review I-11). A fold that would pass 32767 s or 65535 c
   is rejected. (Overstating overcharges coin callers if DW-1
   applies the plan to coin calls; that is why every import is a diff a
   human approves, not an auto-publish.)
4. **Tolerance label**: an imported plan is `exact` only if every row
   used is all-wildcard and the operator attests no fees or overrides
   exist (the response cannot prove it); otherwise `approximate`. The
   label is shown on every preview and carried in events.
5. **Change detection** (from O1, corrected): compare the whole list,
   `count` plus every row's `(telco_id, call_type, carrier, version,
   effective_ts)`, as `source_digest`. Sort the rows by all five fields
   before hashing: `ratesrv.rexx` orders only by `telco_id, call_type,
   carrier`, so rows tied on those come back in no fixed order and an
   unsorted digest would report changes that did not happen. Never key on
   `(telco_id, call_type, carrier)` alone; never parse `effective_ts` as
   seconds (minute precision on the 2.5 line); `version` orders edits.
6. **Fail-safe**: anything but HTTP 200 with `"ok":true`, an empty body
   (BRICKS answers an abended task with an empty 200, KNOWN_BRICKS_QUIRKS
   "Re-verified on 3.4.1"), zero rows, `count` != list length, or a parse
   error keeps the last good plan and raises `rateplan.source_failed`.
7. **Output** is a new **draft** plan version plus a diff against the
   published one. Publishing it is a normal approval (§8.3).

With `cardman_quote` (2.6), steps 2-4 are replaced by sampling CARDMAN's
own computed charge, which removes the lossiness for sampled points;
the plan is still a draft until approved.

### 6.5 Real-time rate request (`0x3f`)

F3 makes mm_manager answer `DLOG_MT_RATE_REQUEST` from a compiled lookup
written alongside the RATE table at publish time: prefix -> entry, one
file per published version, chosen per terminal by its last `0x49`
receipt (F3). It is a file read; it never calls `mm-termcfg` or CARDMAN from inside the
protocol loop (the session is 1200 baud and single-threaded;
`ARCHITECTURE_ANALYSIS.md` §1).

## 7. Deployment (staging and delivery)

### 7.1 Resource: `deployment`

```json
{
  "deployment_id": "dep-20261018-0007",
  "artifacts": [{"kind": "advert_set", "id": "summer-2026", "version": 4},
                {"kind": "rate_plan", "id": "nyc-2026q4", "version": 7}],
  "target": {"scope": "terminal|profile|default", "terminal_ids": ["2125551000"]},
  "terminals": [{"terminal_id": "2125551000",
                 "tables": [{"table_id": "0x1d", "sha256": "...", "state": "delivered",
                             "staged_ts": "...", "delivered_ts": "..."}]}],
  "state": "staging|staged|partially_delivered|delivered|failed|superseded"
}
```

Per-table `state` is a `table_state` (`VOCABULARY.md` §4.6), including
`shadowed` (§3.4).

### 7.2 Staging, per table file

0. Take the F5 locks: exclusive `tables/<tid>/.lock` for a terminal
   target; exclusive `tables/.lock` for a default or profile target
   (which affects every terminal). Steps 2-4 run inside the lock and do
   renames only, so the lock is held for milliseconds; compiling happens
   before. This makes a deployment's set of files change all at once
   from mm_manager's point of view (review A-10).
1. Compile; validate size against the wire struct (`sizeof - 1`, the
   same check `mm_validate_table_fsize()` applies); decode it back.
2. Write `mm_table_XX.bin.tmp-<deployment_id>` in the **same directory**,
   `fsync` the file, `rename()` over the target, `fsync` the directory.
   mm_manager reads a table at download time (`load_mm_table()`), so a
   non-atomic write can send a torn table mid-session.
3. Keep the previous file as `mm_table_XX.bin.prev` (one-step rollback)
   and every published version in `mm-termcfg`'s own store. Rollback
   writes a **fresh copy** through step 2; it never renames `.prev` back,
   because a renamed `.prev` keeps its old mtime and a craft force
   download would skip it (below; independent review I-13).
4. Touch `tables/<tid>/pending` (F2) after all files of the deployment
   for that terminal are in place, never before.

`check_mm_table_is_newer()` compares the table file's mtime with
`table_update.log`'s; a rename keeps the temp file's fresh mtime, so a
craft force download picks the new file up, **for terminal and default
targets only**. That function looks at `tables/<tid>/` and then `-d`;
it never looks at the model profile directory that `load_mm_table()`
uses. A profile-target change is therefore skipped by a craft force
download whenever the `-d` file is older than the terminal's last
download, and needs a lost-memory download, mm_manager's `-c`, or F2.
The deployment says so for profile targets instead of reporting a plain
`staged` (independent review I-13).

A default or profile target's exclusive `tables/.lock` competes with
every line's shared lock; `flock` gives a waiting writer no
priority, so on a busy host it could wait for ever. `mm-termcfg` writes
`tables/.stage-intent` first; F5 makes a line that sees it wait for the
staging to finish (within its own 5 s / 60 s bound) before taking its
shared lock. A stage that still cannot lock within 10 minutes fails
visibly (`failed`, reason `lock_timeout`).

### 7.3 Delivery

A table is `delivered` only when F1 records a receipt whose sha256
equals the staged file's. Until F2 is verified and enabled, delivery
needs a terminal-initiated table update (craft force download, install,
memory loss). The API reports `staged` honestly in the meantime; it
never infers delivery from a call-in.

### 7.4 Drift

A periodic sweep hashes every managed `.bin` file. A hash that matches
no staged version is `drift` (a hand edit or a script run against the
store): reported, never silently overwritten; the operator re-deploys or
adopts the file as a new version.

## 8. Config plane API

### 8.1 Transport and identity

- Default listener: a Unix socket (0660, group `mm-ops`) for the local
  CLI. The caller's uid is read from the socket (`SO_PEERCRED`, or
  `getpeereid()` on macOS/BSD) and mapped to a named identity and its
  roles; an unmapped uid gets nothing, so no change is anonymous
  (review A-12). Optional HTTPS listener, off by default, for remote
  tools and other apps, reached directly or through SNI passthrough only:
  a proxy that terminates TLS would end mTLS and lose the client's
  identity (CARDMAN ADR 42's reason for passing WAPI through).
- HTTPS clients authenticate with **their own** mTLS client cert from a
  config-plane CA that is separate from CARDMAN's CA (that CA is
  CARDMAN's identity system, CARDMAN ADR 42; issuing config certs from
  it would let a config client present a cert CARDMAN trusts). One
  identity per person or app, never a shared one (CARDMAN ADR 13
  decision 1). The cert CN is the actor in the audit log.
- Roles (assigned per identity in `mm-termcfg`'s own config):
  `viewer`, `adverts.edit`, `adverts.approve`, `rates.edit`,
  `rates.approve`, `deploy`, `events.subscribe`, `admin`.
- Rate limiting: per identity, fixed window, default 60 writes / 60 s;
  reads unlimited on the Unix socket.
- The HTTPS listener must not share a port or a TLS listener with
  anything on the card plane, and is never exposed through the same
  reverse proxy hostname as CARDMAN WAPI.

### 8.2 Endpoints (v1)

All bodies JSON. Writes take `If-Match: <etag>` (optimistic
concurrency) and `Idempotency-Key` (replay returns the first result).
Errors use the vocabulary error envelope (`VOCABULARY.md` §7).

| Method and path | Role | Effect |
|---|---|---|
| `GET /v1/terminals`, `GET /v1/terminals/{terminal_id}` | viewer | inventory, current table hashes and the advert set / rate plan versions they correspond to, last call-in, pending deployments |
| `PUT /v1/terminals/{terminal_id}`, `DELETE ...` | admin | enroll or retire a terminal; rewrites the bridge allow-list (§3.2 item 7) |
| `GET /v1/advert-sets`, `GET /v1/advert-sets/{id}[/versions/{v}]` | viewer | read |
| `PUT /v1/advert-sets/{id}` | adverts.edit | new draft version |
| `POST /v1/advert-sets/{id}/versions/{v}/preview` | viewer | compiled sha256, decoded round-trip, rendering |
| `POST /v1/advert-sets/{id}/versions/{v}/approve` | adverts.approve | draft -> approved (§8.3) |
| `GET /v1/rate-plans...`, `PUT /v1/rate-plans/{id}` | viewer / rates.edit | as adverts |
| `POST /v1/rate-plans/{id}/import` `{"kind":"cardman_rates"|"operator_file", ...}` | rates.edit | new draft from a source (§6.4), with diff |
| `POST /v1/rate-plans/{id}/versions/{v}/approve` | rates.approve | |
| `POST /v1/deployments` | deploy | stage approved artifacts to a target |
| `GET /v1/deployments/{id}` | viewer | per-terminal, per-table state |
| `POST /v1/deployments/{id}/rollback` | deploy + the artifact's approve role | re-stage the previous versions; reason required |
| `GET /v1/audit?since=...` | admin | audit records |
| `POST /v1/subscriptions`, `DELETE /v1/subscriptions/{id}` | events.subscribe | webhook subscriptions (§9) |
| `GET /v1/health` | viewer | store writable, last import, relay lag, spool drops |

### 8.3 Approval

Draft -> approved -> published (by a deployment) -> retired (when a
newer version of the same artifact is published). A retired version
cannot be deployed except through rollback (review A-15). Approver must differ
from author (two-person rule) unless the deployment is configured
single-operator (decision DW-3), in which case self-approval is allowed,
needs a typed reason, and is marked `self_approved:true` in the audit
log and events. Content-policy flags (§5.2) and `lossy`/`approximate`
rate entries always require an explicit acknowledgement in the approval
body.

### 8.4 Audit

Append-only log owned by `mm-termcfg`: actor, action, artifact and
version, before/after content hashes, approval reason, source digest for
imports, client address. Never contains card data (there is none on this
plane) or secrets. Retained with the published artifacts.

## 9. Integration with other apps (events)

- The relay reads the bridge's masked spool, the soft modem shim's
  spool when that is deployed (`../softmodem/SPEC.md` §4.5; S1 only), and
  `mm-termcfg`'s own events. It wraps each as a CloudEvents 1.0 JSON
  event in the vocabulary (`VOCABULARY.md` §5) and POSTs it to each
  subscriber.
- Each delivery is signed: `PIV-Signature: t=<unix>,v1=<hex HMAC-SHA256 over "t.body">`
  with a per-subscription secret; receivers reject a `t` older than
  300 s.
- At-least-once delivery with the event `id` as the dedupe key;
  exponential backoff, a dead-letter list visible in `/v1/health`.
  Events are informational. They are not billing records; CARDMAN is the
  only money record, and a consumer that sums events must dedupe on `id`
  (review A-4).
- Subscriber URLs must be HTTPS. Loopback, link-local and private
  address ranges are refused unless an `admin` adds that exact URL to a
  local allow-list, and the bridge's listener and every CARDMAN port are
  always refused. The relay resolves the host once, checks the address,
  connects to that address, and does not follow redirects (no DNS
  rebinding, no redirect to an internal target; review A-11).
  Subscriptions are created only by an identity with `events.subscribe`;
  the URL is never taken from event content.
- No S3 field leaves the host, and S2 fields leave only masked unless
  the subscriber holds `events.sensitive` (`VOCABULARY.md` §3). The relay
  re-validates every outgoing event against the classification **and**
  scans every string value for 13-19 digit runs that pass a Luhn check;
  either hit drops the event and counts it, so a bug upstream cannot leak
  a PAN through an integration, even inside free text (review A-14).

Other front ends (the Asterisk AGI, a future Tranz gateway, the CARDMAN
issue station) can describe their own traffic in the same vocabulary;
the vocabulary is a shared language, not a shared credential. None of
them gain any access to this host's planes by adopting it.

## 10. BRICKS and CARDMAN considerations

### 10.1 BRICKS facts this design depends on

| Fact (source) | Consequence here |
|---|---|
| Route ACL and Basic-Auth are checked by closed-source `bricksd` before any program runs; no lockout on Basic-Auth failures (CARDMAN ADR 14, ADR 18) | A leaked bridge password is not throttled by CARDMAN. Custody (file 0600, never logged) and one identity per deployment (revocable by one `users.conf` row) are the controls. |
| Service identities are telco-global (ADR 38) and `CAUT`/`CDRI` are rate-limit exempt (ADR 18) | `payphone` can authorize against any telco's cards and has no CARDMAN rate limit. The bridge applies its own terminal allow-list and attempt limits (§3.2 items 7-8, review A-1). |
| An abended task answers WAPI with HTTP 200 and an empty body (KNOWN_BRICKS_QUIRKS, 3.4.1) | Both planes treat an empty or unparseable 200 as failure: decline (CAUT), retry/withhold (CDRI), keep last good (rates). |
| JSON is read with `JSONGET`; there is no JSON builder; integers past 2^53 lose precision; CARDMAN's JSON reader does not unescape strings (gateway contract §5 `cdr_id`) | No `"` or `\` in any string sent to CARDMAN; ids stay ASCII `[A-Za-z0-9:_-]`; no integer above 2^53. |
| `effective_ts` and four other served timestamps have minute precision on 2.5 (API_CONTRACTS header) | Vocabulary `ts` accepts minute precision on input from CARDMAN; never orders by it. |
| No WAPI query parameters in practice (`QUERYPARM` unverified, API_CONTRACTS `MTBX`) | Requests to CARDMAN never rely on query strings. |
| WAPI routes are hub-bound on TERMREG (ADR 26); ADMINREG/FRAUDREG ports are not to be fronted (ADR 13 decision 3) | Bridge egress allow-list names the TERMREG WAPI port only. |
| Transids are exactly 4 chars `A-Z0-9` (3.4 strictness); every new route needs `RATELIMIT` wiring or a reviewed exemption (ADR 18), a telco-scope decision (ADR 38), an ACL subset check for LINK targets (ADR 15) | Shapes the requests in §10.2: each is small and names its gates. |
| `RATE` is limited to 30 requests / 60 s per actor (API_CONTRACTS) | Scheduled pulls at most once per 5 minutes; one request per pull. |

### 10.2 Requests to CARDMAN (continuing `CARDMAN_REQUESTS.md` R1-R8)

| # | Request | Why |
|---|---|---|
| R9 | A **read-only `ratesync` group** for `GET /api/rates` only. The edits are the ones ADR 40 made for `payphone`: the `RATE` row in `web_routes.conf`; the `RATE` line's ACL in `transactions.conf` (today `mediation,admin`); the LINK targets `RATE` calls, `ACTR` and `RLIM`, so `acl-lint.sh` passes (ADR 15); a `users.conf` row and a client cert, CN = userid = `rs-<env>`. `ratesrv.rexx` stamps every caller `mediation:<CN>`, so either accept that stamp for `ratesync` or add a `ratesync:` prefix (a program change). Response bytes do not change; `RATE` keeps its `RATELIMIT` wiring (30 / 60 s). | Lets `cardman_rates` sync run without `mediation` (which grants `CDRI`) or `admin`. Smallest grant that does the job (ADR 15's rule). |
| R10 | When `QUOT` lands (2.6), allow the `ratesync` group as well as `payphone`. | Keeps the config plane off the card-plane identity, so revoking one does not cut the other (the reason ADR 40 gave `payphone` its own group). |
| R11 | Document that `GET /api/rates` rows are not unique on `(telco_id, call_type, carrier)` and add `distance_band`/`rate_period` to the 2.6 full-tariff export or quote. | The import must refuse ambiguous rows today (§6.4 step 1). |
| R12 (optional) | Grant `payphone` the existing war-dial limiter `WDIA` (`POST /api/agi/channel-limit`, today `agi,gate`), with `channel_id` = `terminal_id`. The edits, as for R9: the `WDIA` row in `web_routes.conf`, the `WDIA` line in `transactions.conf`, and the ACL of `RLIM`, which `WDIA` LINKs and which does not list `payphone` today (`ACTR` already does), or `acl-lint.sh` fails (ADR 15); plus a reviewed `RATELIMIT` posture for the new caller (ADR 18). | A CARDMAN-side counter per terminal, as an alternative or addition to the bridge's local limit (§3.2 item 8). Note `WDIA` fails open on counter trouble, so the bridge-local limit stays. The first draft understated the edits, as A-13 found for R9 (independent review I-18). |
| R6 (existing) | Quote route | Exact displayed prices. |
| R4 (existing) | International flag on CDRI | Unchanged; affects billing, not this design. |

None of these is required for phases W1-W3 (§12).

## 11. Security model

Trust boundaries:

1. Terminal <-> mm_manager: phone line; `terminal_id` is a claim
   (THREAT_MODEL boundary 1). The config plane targets terminals by that
   claim; that is fine for display data, and no money decision uses it.
2. mm_manager <-> mm-bridge: same host, loopback/Unix socket. Card data.
3. mm-bridge <-> CARDMAN: mTLS + Basic, `payphone`.
4. mm-termcfg <-> operators/apps: per-identity mTLS (or Unix socket
   permissions), roles, audit.
5. mm-termcfg -> table store -> mm_manager -> terminal: integrity only
   (what callers see and pay). Atomic writes, hashes, receipts.
6. mm-termcfg -> subscribers: masked events, signed.

The full threat list and how each item is handled is
[`ADVERSARIAL_REVIEW.md`](ADVERSARIAL_REVIEW.md).

## 12. Phasing

| Phase | Content | Depends on |
|---|---|---|
| W0 | Owner decisions DW-1 to DW-6 (done 2026-10-18, see the ADR); hardware checks H-W1 to H-W6 added to `HARDWARE_TEST_PLAN.md` | - |
| W1 | Production `mm-bridge` (T16 port + §3.2 additions), masked spool. The allow-list file is hand-maintained until W2 writes it. | T16 owner approval |
| W2 | `mm-termcfg` core: store, audit, identities, advert sets, deployments, F1 receipts | W0 |
| W3 | Rate plans: manual and `operator_file` import, compile, F3 local `0x3f` answers | W2, DW-1 |
| W4 | F2 call-in delivery | H-W1 passes |
| W5 | `cardman_rates` scheduled import | R9 granted |
| W6 | Event relay and subscriptions | W1, W2 |
| W7 | `cardman_quote` sampling | CARDMAN 2.6 `QUOT`, R10 |

## 13. Acceptance

- **Card plane**: `tools/cardman_live_test.sh` passes against the
  production bridge; the leak check in `AI/WORKFLOW.md` step 3 finds no
  PAN/PIN in the bridge's logs, spool or health output; a non-CARDMAN
  egress attempt is refused; an unlisted terminal and a missing
  allow-list both decline without a CARDMAN call; the per-card,
  per-terminal and fleet attempt limits behave as §3.2 item 8 says (the
  fleet limit never refuses the whole fleet) and never limit CDRs;
  `card_ref` reaches only `events.sensitive` subscribers, differs
  between two subscriptions for the same card, and changes after a key
  rotation.
- **Table sets**: a deployment staged while a (playback) download session
  holds the F5 lock is applied only after the session; the session sends
  one consistent set. A download cut after the RATE ACK leaves a
  receipt for RATE only, the deployment shows `partially_delivered`, and
  the compiler refuses a version that breaks index stability (§6.2).
- **Receipts**: a playback download writes one F1 receipt per ACKed
  table, keyed by the file's hash, and a `failed` line for a NACK.
- **Managed set**: the compiler refuses table id `0x16`; `mm-termcfg`'s
  user cannot rename over an unmanaged file in a sticky table directory.
- **Adverts**: an approved set reaches a test terminal and its receipt
  hash equals the staged hash; each rejection rule in §5.2 has a test;
  a torn-write test (kill during staging) leaves the previous table
  intact.
- **Rates**: a published plan's entries round-trip through `mm_rate`;
  for an all-wildcard CARDMAN tariff with no fees, the terminal's
  reported `call_cost` equals CDRI `charged_cents` over a fixed call
  matrix; otherwise the measured gap is recorded on the plan.
  Every fail-safe in §6.4 step 6 has a test.
- **Events**: an event with an S3 field is dropped by the relay (test
  injects one); signatures verify; replays older than 300 s are rejected
  by the reference receiver.
- **Honest status**: until H-W1 passes, the API never shows `delivered`
  without a receipt.

## 14. Hardware checks (to add to `HARDWARE_TEST_PLAN.md`)

| # | Check | Why |
|---|---|---|
| H-W1 | Does a terminal accept a table download started by the manager during a routine `DLOG_MT_CALL_IN` session (F2)? Does it apply it? | Without it, delivery needs a technician. |
| H-W2 | VFD character set and the meaning of `display_time` 0 and attributes 17-21. | Text and effect validation in §5.2. |
| H-W3 | Does the terminal's card-call `call_cost` follow the RATE table entries (DW-1)? | Whether CARDMAN-sourced prices change what card callers are shown. |
| H-W4 | Which calls make the terminal send `DLOG_MT_RATE_REQUEST`? | Scope of F3. |
| H-W5 | Cut the line after the RATE ACK in a forced download. Does the terminal keep the new RATE with the old prefix tables, discard the partial download, or ask again (power-lost reason)? Same for a NACKed table. | Whether index stability (§6.2) is the only protection against a mixed set on the terminal (independent review I-2). |
| H-W6 | Does the modem report the calling line's caller ID to mm_manager, and does it match the `terminal_id` in the session? | A check an emulator cannot pass by typing a payphone's number into a packet (§3.2 item 8, independent review I-6). |

### 14.1 Blocker: no way to make physical test cards yet

CARDMAN can mint a test card's number and PIN (`NEWC`), but nothing
today puts that number on a card a terminal can read. CARDMAN's issue
station, which would drive a stripe writer, is proposed only (CARDMAN
ADR 41, `docs/frontends/ISSUE_STATION.md`, not built), and this fork has
no issuance path. Until one exists:

| Needs a physical CARDMAN card | Does not |
|---|---|
| H-W3; every card-path step in `HARDWARE_TEST_PLAN.md` (sections 0-3); the card-plane hardware acceptance in §13; any attempt-limit or allow-list check done from a real terminal | H-W1, H-W2, H-W4 (check with a coin call first), H-W5, H-W6; everything on the config plane (adverts, rate tables, staging, delivery receipts); card-plane tests by `.dlog` playback and against the stub or live CARDMAN (`tools/cardman_live_test.sh`) |

Ways to unblock it, none chosen yet: (a) build the CARDMAN issue station,
or just its stripe-encoding step (Track 2 layout in CARDMAN
`docs/frontends/ACCESS_METHODS_TRACK2.md`); (b) encode a `NEWC`-issued
test card by hand with a stand-alone magstripe writer, keeping the
security rules (test cards only, PIN never written down with the card,
captures deleted after the session); (c) keyed card-number entry at the
terminal, if the card table allows it (the CDR `MANUAL_CARD_NUMBER` flag
suggests the terminal supports it; unverified, and keyed entry may not
exercise the same path as a swipe). This is extra work outside this
spec's phases W1-W7.

## Appendix A. CARDMAN WAPI routes considered

All 49 rows of CARDMAN `bricks/runtime/web_routes.conf` at `9997d87`.
"Plane" says who in this design calls the route; "-" means nobody.

| Route | Transid | Groups today | Plane | Why |
|---|---|---|---|---|
| `POST /api/card/authorize` | `CAUT` | platform, agi, gate, payphone | card | Authorization (ADR 40). |
| `POST /api/cdr` | `CDRI` | mediation, agi, gate, payphone | card | Call records; also the only hold release for `payphone`. |
| `GET /api/rates` | `RATE` | mediation, admin | config | Rate source, via `operator_file` now or `ratesync` after R9. Never on the payphone identity. |
| `POST /api/agi/channel-limit` | `WDIA` | agi, gate | - | Optional R12. |
| `POST /api/card/capture` | `CCAP` | platform, gate | - | `CDRI` captures; a second capture path would bypass ADR 8 rating. |
| `POST /api/card/void` | `CVOD` | platform, gate | - | Not granted to `payphone` (ADR 40); release CDR used instead. |
| `POST /api/card/balance` | `CBAL` | platform, csr, agi, gate | - | The DLOG auth response cannot carry a balance (G6); requesting it would widen `payphone` for nothing the terminal can show. |
| `POST /api/newcust`, `/api/holder`, `/api/card`, `/api/card/load`, `/api/card/pin`, `/api/card/close` | `NEWC`, `HNEW`, `CNEW`, `CLOD`, `CPIN`, `CVOA` | issuer (+csr, gate) | - | Issuance and card admin happen at the issue station (CARDMAN ADR 41), not at a payphone. |
| `POST /api/card/inquiry`, `/api/holder/inquiry`, `/api/card/history` | `CINQ`, `HINQ`, `CHIS` | platform, csr (`CINQ`); csr (`HINQ`, `CHIS`) | - | CSR tools. |
| `POST /api/bill/run`, `/api/bill/inquiry`, `/api/report/daily`, `/api/report/balance`, `/api/settle/run`, `/api/report/settlement`, `/api/report/billing-summary`, `/api/report/revenue`, `GET /api/metrics/business` | `BILW`, `BINQ`, `RPTD`, `RPBW`, `SETW`, `RPSW`, `RCBW`, `RPTR`, `MTBX` | issuer, admin, csr (`BINQ`), gate (`MTBX`) | - | Back-office reporting. Payphone traffic is visible there through `source:"mm_manager"` CDR rows; this design adds no reporting route. |
| `POST /api/sweep/expiry`, `/api/archive/run`, `/api/recon/run`, `/api/cdr/mediate` | `EXPW`, `ARCW`, `RCOW`, `CDMW` | admin | - | Housekeeping. |
| `POST /api/bricks/probe`, `/api/bricks/version-check`; `GET /api/probe/*` (6) | `BPRB`, `BVCK`, `PBHA`, `PBEN`, `PBWO`, `PBVP`, `PBWE`, `PBTS` | admin | - | BRICKS capability probes. |
| `POST /api/{lint,csvk,seed,conc,rgat,igat,sgat,agat,tgat,cgat,fgat,mgat}/run` | `LINT` ... `MGAT` (12) | gate | - | CI gates; the gate cert exists only in CI. |

3270-only transactions with no route, relevant here: `RADM` (tariff
admin: where rates change, so where a rate-plan import's source
changes), `CDRS` (sweep that settles parked `UNMATCHED`/`NOTARIFF` CDRs),
`FRAD` (fraud review, where a payphone's `approved_limited` or
fraud-declined attempts show up).

## Appendix B. DLOG messages and tables this design touches

| Id | Name (`src/mm_manager.h`) | Direction | Used by |
|---|---|---|---|
| `0x02` | `DLOG_MT_FUNF_CARD_AUTH` | terminal -> manager | card plane `/authorize` (existing) |
| `0x41` | `DLOG_MT_AUTH_RESP_CODE` | manager -> terminal | verdict (existing) |
| `0x35` | `DLOG_MT_CALL_DETAILS` | terminal -> manager | card plane `/cdr` (existing) |
| `0x01`, `0x04`, `0x3d`, `0x46` | legacy auth, CD call details, coin call details, Mondex | terminal -> manager | not handled (G4, task T6); out of scope here |
| `0x08` | `DLOG_MT_CALL_IN` | terminal -> manager | F2 delivery trigger |
| `0x2c` | `DLOG_MT_ATN_REQ_TAB_UPD` | terminal -> manager | existing download trigger |
| `0x0e` | `DLOG_MT_TABLE_UPD_ACK` | terminal -> manager | F1 receipts |
| `0x3f` / `0x40` | `DLOG_MT_RATE_REQUEST` / `RESPONSE` | both | F3 |
| `0x1d` | `DLOG_MT_ADVERT_PROMPTS` | manager -> terminal | advert sets |
| `0x49` | `DLOG_MT_RATE_TABLE` | manager -> terminal | rate plans |
| `0x96`, `0x97` | NPA SBR, INTL SBR | manager -> terminal | rate plans (prefix map, international) |
| `0x4a`-`0x51`, `0x5a`, `0x5b`, `0x65`-`0x73`, `0x88`-`0x95`, `0xa0`, `0xa1` | LCD / compressed LCD / NPA-NXX tables | manager -> terminal | rate plan prefix map (which tables a terminal gets depends on its MTR table list) |
| `0x0a`, and `DLOG_MT_SW_VERSION` | `DLOG_MT_TERM_STATUS`, software version | terminal -> manager | inventory via F1 `terminal.json` |

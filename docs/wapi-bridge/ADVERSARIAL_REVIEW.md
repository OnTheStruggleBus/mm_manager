# Adversarial review: WAPI bridge ADR, spec and vocabulary

Reviewed: the first draft of [`ADR-0001-WAPI-BRIDGE.md`](ADR-0001-WAPI-BRIDGE.md),
[`SPEC.md`](SPEC.md) and [`VOCABULARY.md`](VOCABULARY.md), 2026-10-18.
Method: for each plane, assume an attacker or a failure and try to make
the design lose money, leak card or caller data, show callers something
false, or report a state that is not true. Each claim the draft made
about mm_manager or CARDMAN was re-checked against the code or the
CARDMAN docs at the commits in `SPEC.md`'s header.

**Limit of this review**: it was done by the same author in the same
session as the draft. It is a structured red-team pass, not an
independent one. An independent pass by a separate session follows in
[the second section](#independent-pass-2026-10-18).

Severity: **B** blocks the design as written, **H** high, **M** medium,
**L** low. Disposition: **Fixed** (the spec now says something
different; section named), **Accepted** (risk stays, reason given),
**Owner** (needs a decision), **Hardware** (needs a terminal to settle).

## Summary

| Sev | Found | Fixed | Accepted | Owner / Hardware |
|---|---|---|---|---|
| B | 1 | 1 | 0 | 0 |
| H | 5 | 5 | 0 | 0 |
| M | 14 | 11 | 2 | 1 |
| L | 8 | 2 | 6 | 0 |

The one **B** finding: staging could hand a terminal a mixed set of old
and new rate tables in one download (A-10). The draft's atomic-rename
rule protected each file but not the set.

## Card plane (`mm-bridge`)

| # | Sev | Attack or failure | Disposition |
|---|---|---|---|
| A-1 | H | **A fake terminal can lock real customers' cards.** `terminal_id` is an unauthenticated claim (THREAT_MODEL boundary 1). Anyone with a modem and a DLOG emulator (mm_manager itself can be one) can dial in and submit authorizations at line rate. `payphone` is exempt from CARDMAN rate limiting (ADR 18) and telco-global (ADR 38). Three wrong PINs lock a card (CARDMAN `pin_fails`), so walking the `9149 PPP D AAAAAAA C` space with bad PINs locks customers out. The draft added no bridge-side control. | **Fixed** (`SPEC.md` §3.2 items 7-8): the bridge refuses terminals not on an allow-list (written by `mm-termcfg` from explicit admin enrollment, never from a call-in, read-only to the bridge) and enforces a per-terminal and a global invalid-attempt limit, answering `invalid` locally without calling CARDMAN once tripped. Option R12 (grant `payphone` the existing `WDIA` limiter) recorded as an alternative in §10.2. |
| A-2 | M | **The event spool is a new on-disk sink next to card data.** Rule 4 forbids new raw-byte outputs; the spool is not raw bytes, but nothing in the draft set its permissions or proved it free of PAN/PIN. | **Fixed** (§3.2 item 4): spool mode 0640, owner bridge user, group the relay; the `AI/WORKFLOW.md` step 3 decode-and-search leak check covers the spool; schema-checked on write. |
| A-3 | M | **Masked PAN in events is a cardholder tracking key.** `first6****last4` is nearly unique within a telco (last 4 = 3 dial_id digits + check digit). Third-party subscribers would get per-cardholder call trails with terminal and time. | **Fixed** (`VOCABULARY.md` §3, §4.1, §5): events carry `card_ref`, an HMAC-SHA256 of the PAN under a bridge-held key (the bridge already keeps `HMAC(card)` for its `requested_at` cache), truncated to 16 hex. `masked_pan` moves to `events.sensitive` subscribers only. |
| A-4 | M | **Events could be treated as billing records** and double-count under at-least-once delivery. | **Fixed** (§9, `VOCABULARY.md` §5): events are informational; CARDMAN is the only money record; dedupe on `id` is mandatory for consumers. |
| A-5 | L | `GET /healthz` lets any local process see last success times. | **Accepted**: no card data; same host, same trust as the loopback card listener. |
| A-6 | L | A restart of both mm_manager and the bridge inside 600 s loses `requested_at` and can place a second hold on a retry. | **Accepted**: existing, documented in `BRIDGE_CONTRACT.md` §3 and `CARDMAN_REQUESTS.md` R1; this design does not change it. |

## Config plane (`mm-termcfg`) and table delivery

| # | Sev | Attack or failure | Disposition |
|---|---|---|---|
| A-10 | **B** | **Mixed-version table set mid-download.** mm_manager opens each table file when it sends it (`load_mm_table()`), in list order: RATE (`0x49`) before the NPA-NXX tables (`table_list_mtr_*`). A deployment staged while that terminal is in a download session can send old RATE with new NPA-NXX tables, whose entries point at RATE indices that mean something else: wrong prices on a live phone. Per-file atomic rename does not prevent it. | **Fixed** (§3.1 F5, §7.2): mm_manager holds an exclusive `flock` on `tables/<tid>/.lock` (and a shared lock on `tables/.lock` for default/profile files) for the whole download; `mm-termcfg` stages under the same locks and waits; the staging critical section is file renames only. mm_manager waits at most 5 s for the lock, then skips the download for this session and logs it (it never sends a set it could not lock). |
| A-11 | H | **SSRF through webhooks.** The draft allowed loopback subscriber URLs. A `events.subscribe` identity could aim the relay at the bridge (`127.0.0.1:8420`), CARDMAN's WAPI or other internal services. | **Fixed** (§9): HTTPS only; loopback, link-local and private ranges refused unless an `admin` adds that exact URL to a local allow-list; the bridge and CARDMAN ports are always refused; the relay resolves once and connects to the checked address (no DNS rebinding); redirects are not followed. |
| A-12 | H | **Unix socket users are anonymous.** The draft gave group `mm-ops` the socket with no per-user identity, so every change over the socket would be unattributed ("no anonymous state changes", CARDMAN principle 6). | **Fixed** (§8.1): the peer's uid is read from the socket (`SO_PEERCRED` / `getpeereid`) and mapped to a named identity and roles; an unmapped uid gets nothing. |
| A-13 | H | **R9 was understated.** The draft said granting `ratesync` on `GET /api/rates` was a `web_routes.conf` change only. In CARDMAN the `RATE` transid's own ACL (`transactions.conf`: `mediation,admin`) and its LINK targets `ACTR` and `RLIM` must also list the group, or `acl-lint.sh` fails (ADR 15, the same edits ADR 40 made for `payphone`). `ratesrv.rexx` also stamps the actor `mediation:<CN>`, so a `ratesync` caller would be attributed as mediation. | **Fixed** (§10.2 R9): the request now names all four config lines, the cert/users row, and asks for a `ratesync:` actor prefix or acceptance of the `mediation:` stamp. |
| A-14 | H | **Leaking an S3 value through a free-text field.** The relay's draft filter was by field name. A PAN could still appear inside a string (an error message, a mis-filled advert). | **Fixed** (§9): the relay also scans every string value for 13-19 digit runs that pass Luhn and drops the event; counts drops in `/v1/health`. |
| A-15 | M | **Stale but approved content redeployed.** A `deploy`-only identity could re-publish an old approved advert with an outdated price. | **Fixed** (§8.2, §8.3): superseding a version retires the older ones; retired versions cannot be deployed except by rollback, which needs `deploy` plus the original approver role and records a reason. |
| A-16 | M | **mtime-based freshness breaks on a host clock step.** `check_mm_table_is_newer()` compares file mtime to `table_update.log` mtime. A clock moved backwards makes a new table look old and skips it on a craft force download. | **Fixed** (§3.1 F2): the "is this table pending" decision for F2 compares the staged sha256 with the last receipt's sha256 (F1), not mtimes. Upstream's craft force path is left as is. |
| A-17 | M | **Call-in downloads keep the payphone out of service.** The terminal places the call-in on its own line; a full table list at 1200 baud on every call-in with anything pending can take minutes. | **Fixed** (§3.1 F2): F2 sends only the tables whose staged hash differs from the last receipt, then `END_DATA`. |
| A-18 | M | **Inventory source would put card data on the config plane.** The draft had `mm-termcfg` read mm_manager's SQLite (`TSWVERS`, `TSTATUS`). The same file holds masked PANs (`TAUTH`, `TCDR`) and there is one DB per line process in its working directory. | **Fixed** (§3.1 F1): the receipt hook also writes `tables/<tid>/terminal.json` (model, MTR version, last session time) at session end; `mm-termcfg` never opens mm_manager's database. |
| A-19 | M | **Operator-uploaded "CARDMAN" files are not provable.** `operator_file` trusts the uploader; CARDMAN does not sign responses. | **Accepted** with labelling (§6.3): the plan's source records `operator_attested` and the uploader's identity; approval is still required. |
| A-20 | M | **Advert prices can contradict the rate plan**, misleading callers. | **Accepted** for phase W2 (§5.2): the approver sees both side by side. A machine check is a later improvement. |
| A-21 | M | **"Never understate" overcharges coin callers** if the plan is also the coin price list (DW-1 (a)); the opposite choice shows card callers coin prices. | **Owner** (DW-1); every import is a human-approved diff, never an auto-publish. |
| A-22 | L | A receipt proves the terminal ACKed a table, not that it applied it. | **Accepted**: H-W1 covers apply; a receipt is the best signal the protocol gives. |
| A-23 | L | Putting the HTTPS listener behind a TLS-terminating proxy would end mTLS at the proxy and lose client identity (the reason CARDMAN ADR 42 passes WAPI through by SNI). | **Fixed** (§8.1): direct or SNI passthrough only. |
| A-24 | L | Rebuilding after a lost `mm-termcfg` store. | **Accepted**: the drift sweep (§7.4) can adopt existing table files as new versions. |

## Vocabulary

| # | Sev | Problem | Disposition |
|---|---|---|---|
| V-1 | M | `call_type` meant two things (CALLTYP integer on the bridge hop; `DOM/INTL/TOLLFREE` in CARDMAN). | **Fixed** in the draft already (`dlog_call_type`). Kept as a review item because it is the shape of the next collision. |
| V-2 | M | **`dlog_rate_type` named two different things**: the CDR's `rate_type` byte (meaning unverified) and the RATE entry's `rate_type_t` enum. Violates the vocabulary's own rule 2. | **Fixed**: the CDR field is `dlog_cdr_rate_type`. |
| V-3 | M | **`phone_number` collides with DLOG.** Canonical `phone_number` was the payphone's line, but `dlog_mt_funf_card_auth.phone_number` is the **dialed** number. A mapper reading struct names would cross them. | **Fixed**: canonical is `terminal_phone_number`; CARDMAN `terminal.phone_number` and bridge-hop `phone_number` are listed as aliases; the DLOG auth field is mapped to `called_number` with a warning note. |
| V-4 | L | Event type prefix `org.piv.` implies a domain nobody here owns; CloudEvents asks for a reverse-DNS prefix. | **Fixed**: `io.github.onthestrugglebus.piv.` |
| V-5 | L | `terminal_id` is the line number, so config follows the line, not the device, when a payphone is moved. | **Accepted**: that is how DLOG identifies terminals today; noted in §4.3. |
| V-6 | L | Terminal `call_cost` units assumed cents. | **Accepted**: consistent with upstream's rate answer and the live CDR example; flagged "terminal-reported" in §4.4 and checked by H-W3. |

## Coverage of the request

| # | Sev | Gap | Disposition |
|---|---|---|---|
| X-1 | M | The draft did not show that **every** CARDMAN WAPI route and every relevant DLOG message had been considered, only the ones it used. | **Fixed**: `SPEC.md` Appendix A (all 49 CARDMAN routes, used / not used / why) and Appendix B (DLOG messages and tables this design touches). |

## Claims re-verified against sources

| Claim | Verified where |
|---|---|
| Routine call-in does not download tables | `src/mm_manager.c` `case DLOG_MT_CALL_IN`: download lines commented out |
| Only `ATN_REQ_TAB_UPD` triggers a download | `case DLOG_MT_ATN_REQ_TAB_UPD` sets `table_download_pending` |
| `TABLE_UPD_ACK` is only printed | `case DLOG_MT_TABLE_UPD_ACK` |
| `0x3f` is answered with fixed 240/100/60/25 | `case DLOG_MT_RATE_REQUEST` |
| Advert table 20 x 24 bytes, 480-byte file | `dlog_mt_advert_prompts_t`, `generate_advert_prompts.py` |
| RATE file 1191 bytes, 128 x 9-byte entries | `dlog_mt_rate_table_t`, `src/mm_rate.c` header |
| `RATE` ACL `mediation,admin`; `ACTR`/`RLIM` ACLs; actor `mediation:` | CARDMAN `transactions.conf`, `ratesrv.rexx` at `9997d87` |
| `/api/rates` omits band, period, fees, overrides | CARDMAN `API_CONTRACTS.md` `GET /api/rates` |
| Empty 200 on an abended task | CARDMAN `KNOWN_BRICKS_QUIRKS.md` "Re-verified on 3.4.1" |
| `payphone` rate-limit exempt and telco-global | CARDMAN ADR 18, ADR 38, ADR 40 decision 4 |

---

## Independent pass, 2026-10-18

Reviewer: a separate Claude Code session that did not write the draft or
the first pass. It read the four documents and checked their claims
against this fork at `c8c5c11` (`src/mm_manager.c`, `src/mm_manager.h`,
`src/mm_proto.c`, `src/mm_card.h`, `src/mm_rateint.c`,
`generate_advert_prompts.py`, `config/advert_prompts.csv`,
`tools/cardman_bridge.py`, `docs/cardman-compat/`, `AI/SECURITY_RULES.md`)
and CARDMAN `origin/main` `9997d87` (`API_CONTRACTS.md`,
`MILLENNIUM_GATEWAY_CONTRACT.md`, `ARCHITECTURE_DECISIONS.md` ADR 10, 14,
18, 40, 42, `KNOWN_BRICKS_QUIRKS.md`, `web_routes.conf`,
`transactions.conf`, `ratesrv.rexx`, `wdial.rexx`). Same severity and
disposition scale as above. Fixes are in the documents now; each fixed
place cites the finding number.

**Limit of this pass**: it is still a reading of documents and code, not
a test. Nothing was run against a terminal or CARDMAN. The reviewer is
another session of the same model, not a second person.

### Summary

| Sev | Found | Fixed | Accepted | Owner / Hardware |
|---|---|---|---|---|
| B | 1 | 1 (and H-W5) | 0 | 0 |
| H | 5 | 5 (one also H-W6) | 0 | 0 |
| M | 10 | 10 | 0 | 0 |
| L | 5 | 5 | 0 | 0 |

The **B** finding (I-2) is the same harm as A-10, wrong prices on a live
phone, reached by a more ordinary path: a dropped line part way through
a download. The F5 lock protects the table store, not the terminal.

The first pass's "Claims re-verified" table was right line by line but
missed that during a download the ACKs never reach the code it pointed
at (I-1), and its A-3 fix moved the cardholder trail into `card_ref`
instead of removing it (I-4).

### Findings

| # | Sev | Attack or failure | Disposition |
|---|---|---|---|
| I-1 | H | **F1 receipts would never be written.** The draft hooked receipts to `case DLOG_MT_TABLE_UPD_ACK` in `process_mm_table()`. In a manager-pushed download, `mm_download_tables()` calls `wait_for_table_ack()` (`src/mm_proto.c`), which reads and consumes each ACK itself; `process_mm_table()` never sees them. So nothing would ever reach `delivered`. `mm_download_tables()` also ignores that function's return value. And "sha256 of the bytes actually sent" never matches the staged file for tables mm_manager patches after loading (`DLOG_MT_FCONFIG_OPTS` `term_type`, `DLOG_MT_CALL_SCREEN_LIST` padding). | **Fixed** (`SPEC.md` §3.1 F1, ADR W6): the receipt is written where `wait_for_table_ack()` returns 0, hashes the file as loaded, records the resolved path, and a NACK writes a `failed` line. |
| I-2 | **B** | **A cut-off download leaves a mixed set on the terminal.** `mm_download_tables()` sends RATE (`0x49`) before the prefix tables and does not stop after a failed ACK (`status` is overwritten on the next table). A line drop or a NACK after the RATE ACK leaves the terminal with new RATE and old NPA-NXX/LCD tables. If the new plan moved what an index means, live calls are priced from the wrong entries until the next download, which can be a technician visit away. A-10's lock does not help: the store was consistent; the delivery was not. | **Fixed** (§6.2 "Index stability", §3.1 F5): a version may not change what an index means while the published prefix map points at it; re-pointing takes two deployments, and the compiler enforces it. F5 stops sending managed rate tables after a failed ACK. **Hardware** H-W5: what the terminal does with a partial download. |
| I-3 | H | **The config plane could switch off card validation.** §3.4 made `mm-termcfg` "the only writer of `.bin` files", with write access to the table directories. The card table `0x16` is one of those files, and its `vfy_flags` (`src/mm_card.h`) include `CARD_VF_NCCVAL_IND` (whether the terminal asks the manager to validate a card), `CARD_VF_PROMPT_FOR_PIN` and `CARD_VF_ACCS_ROUTING`. A bug or a compromised config identity could let card calls run with no `CAUT` and no hold, which W1 says the config plane must never be able to cause. | **Fixed** (§3.4, ADR W1): an explicit managed-table list (adverts and rate tables only), the compiler refuses other ids, sticky table directories with unmanaged files owned by the operator account, and the drift sweep alerts on unmanaged files too. |
| I-4 | H | **`card_ref` is the same tracking key A-3 removed.** A-3 found that `masked_pan` gives subscribers per-cardholder call trails. Its fix sent every subscriber `card_ref`, which by design is the same for every call on a card, next to terminal, time, masked called number and `charged_cents`. Hashing hides the digits, not the trail. The key's lifetime was also unspecified: the existing bridge key is random per process and memory-only; persisting it would let whoever holds it reverse any `card_ref` by enumerating one telco's roughly 10^7 PANs. | **Fixed** (`SPEC.md` §3.2 item 4, `VOCABULARY.md` §3, §4.1, §5, ADR W8): `card_ref` is S2, sent only to `events.sensitive` subscribers, re-keyed per subscription so subscribers cannot join on it; the key lives only in memory and rotates every 24 h. Default events carry no card identifier. |
| I-5 | H | **`dial_id` was S2, but it is nearly the PAN.** CARDMAN ADR 10: PAN = `9149 PPP D AAAAAAA C` and `dial_id` = `AAAAAAA`. With the telco's `PPP`, a one-digit product code and the Luhn digit, a `dial_id` gives the PAN in at most ten guesses, and it is what a customer keys at the AGI dial-in. S2 allowed it in masked local logs, next to `masked_pan`, whose last four digits overlap it. | **Fixed** (`VOCABULARY.md` §3, §4.1): `dial_id` is S3; `masked_pan` never shares a record with any part of it; the spool may not carry it (§3.2 item 4). |
| I-6 | H | **The attempt limits do not stop lockouts, and the fleet limit is a kill switch.** CARDMAN locks a card after 3 consecutive bad PINs. At 60 `invalid` per 10 minutes fleet-wide, an emulator still locks 20 cards every 10 minutes. Once tripped, the fleet limit declined every card call on every payphone; terminal ids are printed on the payphones, so 61 bad attempts would stop card service everywhere. The per-terminal limit likewise lets anyone deny one payphone's card service. | **Fixed** (§3.2 item 8, ADR W1): a per-card limit (2 consecutive `invalid`, 24 h) so the payphone path cannot deliver a third strike by itself; the fleet limit alerts and tightens per-terminal limits instead of refusing everyone; the per-terminal denial risk is stated. **Hardware** H-W6: whether the modem's caller ID can confirm the claimed `terminal_id`. |
| I-7 | M | **F3 would quote the newest plan to a terminal holding the old one.** A plan is staged long before it is delivered (until H-W1 passes, until a technician visits). Answering `0x3f` from the newest compiled lookup tells callers prices that the terminal's own RATE table does not charge. | **Fixed** (§3.1 F3, §6.5, ADR W9): one lookup per published version, chosen by the terminal's last `0x49` receipt; no receipt means today's answer and a warning. |
| I-8 | M | **A rate change makes adverts already on the phone false.** The stock `config/advert_prompts.csv` says `Local coin calls` / `50-cents` and `$1.00 for 4-minutes`. A-20 checks prices only when an advert is approved; approving a rate plan never looked at adverts already in effect, so a price change leaves the old price on the display. | **Fixed** (§5.2): rate plan approval and deployment previews list price-bearing slots of the advert sets in effect on the target terminals, including the default set, and the approval must acknowledge them. |
| I-9 | M | **The advert content policy is easy to slip past.** It checked each slot alone for 7+ digits or a `$`. A number split across two slots (`Call 212-555` / `0123 now`), a vanity number (`1-800-CALL-NOW`), or a price in words (`50-cents`, which is in the stock file) passed. | **Fixed** (§5.2): the digit check also runs on each hook state's slots joined in display order, counts letters next to digits as keypad digits, and the price check matches `c`/`cents` and `free`. |
| I-10 | M | **INTL index range was wrong and could silently block or un-price a country.** Flags 2-63 plus 28 are RATE entries 30-91, not 28-91. A `rate_index` of 28 or 29 compiles to flags 0 (NCC rated) or 1 (blocked). The example's `default_rate_index: 0` uses the same encoding (`src/mm_rateint.c`) and meant "NCC rated", not RATE entry 0. | **Fixed** (§6.1, §6.2, `VOCABULARY.md` §4.6): range 30-91, with `"ncc_rated"` and `"blocked"` as explicit values for both countries and the default. |
| I-11 | M | **"Never understate" understated.** Raising `initial_charge` while keeping `initial_period = initial_secs` bills the second block's time at the `addl` rate, which is lower whenever CARDMAN's second block is dearer. | **Fixed** (§6.4 step 3): fold both period and charge (`initial_secs + second_secs`, `initial_cents + second_cents`), flagged `lossy`, rejected on 16-bit overflow. |
| I-12 | M | **The import ignored wildcard telco rows and carrier rows.** `GET /api/rates` returns every telco's rows unscoped, including `*` rows (CAUT's `no_tariff` rule names the universal `'*'/'*'/'*'` row). Selecting only the plan's exact `telco_id` drops the fallback. Several carrier rows for one `call_type` passed the duplicate check and one would be picked arbitrarily; the terminal cannot tell carriers apart. `ratesrv.rexx` orders only by `telco_id, call_type, carrier`, so a digest over rows tied on those flaps between identical pulls. | **Fixed** (§6.4 steps 1 and 5): telco rows, else `*` rows, per `call_type`; the highest price per block across carriers, flagged `lossy`; sort by all five fields before hashing. CARDMAN's exact tie-break is not in its contracts, so the label stays `approximate`. |
| I-13 | M | **Profile and default deployments could sit at `staged` with no reason given, and rollback could be skipped.** `load_mm_table()` prefers `tables/<tid>/`, then the model profile, then `-d`; a per-terminal file shadows a default or profile deployment for good. `check_mm_table_is_newer()` never looks at the profile directory, so a craft force download skips a newer profile table whenever the `-d` file is older than the last download; §7.2's claim that a rename makes the craft force path pick up the new file was false for profile targets. Renaming `.prev` back for rollback restores an old mtime, which that check also skips. | **Fixed** (§3.4, §7.1, §7.2, `VOCABULARY.md` §4.6): deployments resolve the effective file per terminal and report `shadowed`; profile targets say they need a lost-memory download, `-c` or F2; rollback writes a fresh copy. |
| I-14 | M | **F5's skip path and lock fairness.** `process_mm_table()` sends `DLOG_MT_TABLE_UPD` before `mm_download_tables()` runs, so "skip the download" happened after the terminal was told one was coming, a protocol state nobody has seen. For lost memory or power lost on download, skipping leaves the terminal with no tables. And `flock` gives a waiting exclusive locker no priority, so a default-target stage could wait for ever behind overlapping shared locks from many lines. | **Fixed** (§3.1 F5, §7.2): lock before the reply; wait up to 60 s for lost-memory reasons; a `.stage-intent` file gives staging priority; a stage that cannot lock in 10 minutes fails visibly. |
| I-15 | M | **`cardman_quote` offered the card-plane identity to the config plane.** §6.3 listed a "`payphone`-group read grant" as `mm-termcfg`'s credential for `QUOT`. That contradicts W1 (no shared credential), and `payphone` also grants `CAUT` and `CDRI`. CARDMAN's proposed `QUOT` ACL is `payphone,mediation,admin`, which makes the mistake easy to make. | **Fixed** (§6.3): `ratesync` only, via R10. |
| I-16 | M | **`called_number_masked` hid little.** "First 6 digits + `****`" of a 7-digit local number as dialed shows 6 of its 7 digits; for a short number it shows all of it; it was S1, so every subscriber got it with terminal and time. | **Fixed** (`VOCABULARY.md` §4.2, §5): made from the normalized number, keeping only the NPA (NANP) or `011` plus country code; anything else is `****`. |
| I-17 | L | Appendix A listed `HINQ`/`CHIS` as `platform, csr` (both are `csr` only) and the back-office row omitted `gate` on `MTBX`. | **Fixed** (Appendix A). |
| I-18 | L | R12 understated its edits, as A-13 found for R9: `WDIA` LINKs `RLIM`, whose ACL does not list `payphone`, so `acl-lint.sh` would fail; `transactions.conf`'s `WDIA` line and an ADR 18 review are also needed. | **Fixed** (§10.2 R12). |
| I-19 | L | `AI/SECURITY_RULES.md`'s checklist says a valid `auth_code` is "exactly 8 hex chars". The bridge (`AUTH_CODE_RE`), `VOCABULARY.md` and CARDMAN_REQUESTS R3 say base-36 `[0-9A-Z]{8}`. The spec's precedence line ("the security rules win") would pick the wrong alphabet and decline every code with a letter past `F`. | **Fixed** (owner-approved 2026-10-18) in its own PR, branch `docs/auth-code-base36`: the `AI/SECURITY_RULES.md` checklist, `COMPATIBILITY_PLAN.md` O2 item 2 and `AI/TASKS.md` T5 change to 8 chars of `[0-9A-Z]` (base-36), matching `src/mm_cardman_bridge.c` and `AUTH_CODE_RE`. Docs only; the code already checks base-36. |
| I-20 | L | §5.1 said `generate_advert_prompts.py` "checks the size". It writes the file, then only prints a warning, and writes text longer than 20 characters as is. | **Fixed** (§5.1): stated as it is; the compiler enforces sizes itself. |
| I-21 | L | `/healthz` `cardman_reachable` came from the last real call however old, so after a quiet night it reports a state nobody has checked. | **Fixed** (§3.2 item 3): `null` when the last outcome is older than 15 minutes. |

### Claims this pass checked and found right

| Claim | Where checked |
|---|---|
| Routine call-in does not download (lines commented out, and they also set `TTBLREQ_CRAFT_FORCE_DL`, so a re-enabled F2 would be mtime-filtered unless it bypasses that, as F2 says) | `src/mm_manager.c` `case DLOG_MT_CALL_IN` |
| `0x3f` answer 240/100/60/25 outside `rating_test_mode` | `case DLOG_MT_RATE_REQUEST` |
| RATE file 1191 bytes; `FLAG_PERIOD_UNLIMITED` bit 15; `rate_type_t` values | `src/mm_manager.h` |
| Advert entry layout, 20 x 24, attribute codes 0-5 and 17-21, `0x40` blank-before | `src/mm_manager.h`, `generate_advert_prompts.py` |
| 49 WAPI routes; `RATE` ACL `mediation,admin`; `ratesrv.rexx` LINKs `ACTR` and `RLIM`, stamps `mediation:<CN>`, caps 30/60 s, fails closed mid-list | `web_routes.conf`, `transactions.conf`, `ratesrv.rexx` |
| `payphone`: `CAUT` + `CDRI` only, telco-global, rate-limit exempt; actor stamps | ADR 40, gateway contract §2 |
| `/api/rates` omits band, period, fees, overrides; minute-precision `effective_ts` | `API_CONTRACTS.md` |
| Basic-Auth failures are checked in `bricksd` with no lockout; `pin_fails` locks at 3 consecutive | ADR 14 |
| SNI passthrough because mTLS identity must reach `bricksd` | ADR 42 decision 3 |
| Empty 200 on an abended task | `KNOWN_BRICKS_QUIRKS.md` "Re-verified on 3.4.1" |

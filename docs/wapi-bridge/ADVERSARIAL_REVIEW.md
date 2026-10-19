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
independent one. An independent review (a second person, or a fresh
session given only these files and the sources) is still worth doing
before the owner accepts the ADR.

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

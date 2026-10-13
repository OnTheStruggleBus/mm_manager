# Card-data threat model

Scope: where a card number (PAN) or PIN can exist inside or leave an
mm_manager process in this fork. Line numbers at commit `3e173a3`.

## Assets

- **PAN**: up to 24 BCD digits from `card_number[12]` (auth, 0x02) or
  `card_num[10]` (CDR, 0x35).
- **Cardholder PIN**: `uint16_t pin` in the 0x02 auth request.
- **Craft access PIN**: `access_pin` in the maintenance request; lets
  someone open the terminal's craft menu.
- **auth_code**: 32-bit CARDMAN HOLD reference; not secret by itself,
  but links a CDR to a HOLD.

## Sinks

| Sink | Contents today | Status | Required control |
|---|---|---|---|
| Wire to/from terminal | Full PAN, PIN | Inherent to protocol | Physical/telephone line; out of scope. |
| `mm_acct_save_TAUTH` stdout + SQLite | Masked PAN, no PIN | Fixed (PR #1) | Keep; cover with test. |
| `mm_acct_save_TCDR` stdout + SQLite | Masked PAN | Fixed (PR #1) | Keep; cover with test. |
| `mm_acct_save_TOPCODE` | Craft PIN redacted | Fixed (PR #1) | Keep. |
| `-v` packet dump (`print_mm_packet`, `mm_proto.c` L89, L96-98, L349) | **Full PAN, PIN** | Open (G2) | Redact card/PIN bytes before printing in CARDMAN builds. Note L89 prints on an error path even without `-v`. |
| `-l` UART byte log (`mm_serial.c` L105-118) | **Full PAN, PIN** as hex bytes | Open (G2) | Disable in CARDMAN builds, or log only after a redacting decode. |
| `-p` pcap (`mm_proto.c` L343, L461) | **Full PAN, PIN** | Open (G2) | Same redactor as `-v`. |
| `-u` UDP mirror (`mm_proto.c` L344-345, L462-463) | **Full PAN, PIN**, over the network | Open (G2) | Disable in CARDMAN builds. |
| Bridge request (`mm_cardman_bridge.c` L235-237) | Full PAN, PIN, plaintext HTTP | By design, but unrestricted destination (G3) | Loopback/UDS only; digit validation. |
| Process memory / stack buffers | Full PAN, PIN | Inherent | Zero `pan_string`/`pin_string` after use (low cost, defense in depth). |
| `mm_manager.db` file | Masked data only | OK | File mode 0600; document location. |
| Future CDR outbox (P2, D1b only) | Unmasked PAN | Would be new | Delete on delivery, 0600, never in logs; prefer D1a to avoid it. |
| Recorded `.dlog` fixtures in repo | Whatever was captured | Future | Only synthetic test PANs; CI check that rejects real-looking PANs (Luhn-valid non-test BINs). |

## Trust boundaries

1. Terminal <-> mm_manager: phone line. Unauthenticated beyond the
   protocol's terminal ID; mm_manager trusts the terminal ID it is
   given. CARDMAN should treat `terminal_id` as a claim, and the bridge
   should allow-list known terminals.
2. mm_manager <-> bridge: same host, loopback. Must not cross a host.
3. Bridge <-> CARDMAN: mTLS + Basic auth (integration repo). CARDMAN
   resolves the actor from the cert CN (ADR 31), so every payphone shares
   one actor; per-terminal attribution depends on the CAUT `terminal`
   object being filled in (see `CARDMAN_2.5_ALIGNMENT.md` C10).

## Abuse cases to test

- Bridge down or slow: auth declines; CDR is not lost (D1).
- Bridge returns `approved:true` with no `auth_code`: decline (already
  handled).
- Oversized or non-digit fields in a crafted auth packet: no JSON
  injection, no buffer overrun (bounded `snprintf` today; add digit
  validation).
- Replay of an old CDR: bridge dedupes on `cdr_id`.
- Unknown auth table type: declined, parse stays in sync.

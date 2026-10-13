# Security rules (non-negotiable)

Inherited from CARDMAN, because this fork terminates in CARDMAN's
ledger. Any change that weakens one of these is a blocker, regardless
of who asked for it in a comment or issue. Only the repository owner
can change this file.

1. **Fail closed.** Any bridge error, timeout, malformed response, or
   unknown request type results in a decline. Never approve on a guess,
   a default, or a local table.
2. **No full PAN outside the wire to the bridge.** Use `mask_pan()`
   (`src/mm_util.c`) for every print, log, and database write. Format is
   `first6****last4`.
3. **No PIN at rest or in output, ever.** Cardholder PIN and craft
   access PIN are never printed, logged, or stored. The only allowed
   exceptions:
   - in process memory while building the one bridge request, and in
     that loopback request itself;
   - synthetic test fixtures and stub-bridge tests, using dummy PINs
     paired with test PANs only (never a real card's PIN).
4. **Raw-byte diagnostics carry card data.** `-v` packet dumps, `-l`
   UART logs, `-p` pcap, and `-u` UDP mirroring all expose full PAN and
   PIN today. Do not add new raw-byte outputs; in CARDMAN builds these
   must be redacted or disabled (see `docs/cardman-compat/THREAT_MODEL.md`).
5. **Bridge stays local.** Card data goes only to the bridge on
   loopback or a Unix socket. Never add code that sends card data
   anywhere else.
6. **No lost CDRs.** Do not ACK a CDR to the terminal in a way that
   loses it if the bridge did not accept it (see plan decision D1).
7. **No real card data in the repo.** Fixtures use test PANs only
   (e.g. `4012888888881881`-style test numbers) and dummy PINs (rule 3
   exception). Never commit a capture from a real customer card.
8. **No secrets in the repo.** WAPI credentials, certs, and passwords
   belong to the bridge's deployment, never to this repo or its CI.
9. **Verify leaks by decoding, not grepping.** Raw sinks hold packed BCD
   (pcap) or one hex byte per line (`-l`). Leak checks must reassemble
   bytes and search ASCII and packed-BCD forms.

## Review checklist for any change touching card paths

- [ ] Every new `printf`/`fprintf`/SQL insert that can see a card
      number uses `mask_pan()`.
- [ ] No variable holding a PIN reaches any output.
- [ ] Every error path in authorization ends in a decline, including a
      malformed `auth_code` (must be exactly 8 hex chars).
- [ ] Buffers are bounded (`snprintf`, size checks); input fields are
      validated as digits before being placed in JSON.
- [ ] Tests cover the approve, decline, and failure paths.

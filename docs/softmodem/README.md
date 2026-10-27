# Soft modem

Architecture decision and integration spec for answering the terminal's
dial-in with an open-source soft modem instead of a hardware modem.
Docs only; nothing here is built yet.

| File | What it covers |
|---|---|
| [`ADR-0002-SOFT-MODEM.md`](ADR-0002-SOFT-MODEM.md) | Selection criteria, the evidence (nineteen candidates checked against their source and licence files, across three search passes), the decision (datamodem, standalone, behind a fork-side DTE shim), owner decisions DS-1 to DS-5, and the design review (H1-H2, M1-M4, L1-L2, all dispositioned). Start here. |
| [`SPEC.md`](SPEC.md) | Topology, ATA/Asterisk/datamodem configuration, the `mm-softmodem-dte` shim contract, call flow, instances/migration/recovery (§3.6), shim states incl. `COOLDOWN`, health signals (§4.5), security controls C-1 to C-7, licence and supply chain, tests T-1 to T-7 and hardware checks SM-0 to SM-10, phasing (U2 on the critical path), upstream requests U1-U3. |

Key finding: datamodem, the selected modem, answers V.22 with the 2100
Hz tone, not Bell 212A's 2225 Hz, so whether the Millennium accepts it
is settled on hardware first (SPEC §9.2). Only one open project answers
with a real Bell 2225 Hz tone: el-modem, which is experimental and
x86-64 only. It is the designated alternate for that test. The
hardware modem stays the supported fallback.

Cross-reference with the terminal's own modem (ADR "The Millennium's own
modem"): the terminal's modem chip is a 73K212 (Bell 212A/103 only,
which cannot detect 2100 Hz) or a 73K324L. However, its MODEM table
(DLOG `0x21`) has an "answer tone NOT required (use carrier only)"
setting that mm_manager can download per terminal (SPEC §3.1a). Each
terminal still needs one bootstrap call-in through a 2225 Hz answerer.

Replaces the unevaluated `app_softmodem -> TCP -> socat PTY` box in
[`../cardman-compat/COMPATIBILITY_PLAN.md`](../cardman-compat/COMPATIBILITY_PLAN.md) §1.

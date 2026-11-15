# ADR-0003: Built-in SIP soft modem (imported from jxburros/mm_manager)

- **Status**: Accepted, 2026-11-15 (owner). Supersedes
  [ADR-0002](ADR-0002-SOFT-MODEM.md) (datamodem plus a DTE shim).
- **Scope**: the modem leg between a Nortel Millennium terminal and
  mm_manager. No change to the card path, the bridge contract, or ADR-0001.
- **Setup**: [`PHREAKNET.md`](PHREAKNET.md).

## Context

ADR-0002 chose datamodem (GPL-3.0+) behind a fork-side shim. Its review
found the blocker that shaped everything after it: datamodem answers V.22
with the 2100 Hz V.25 tone, while the terminal's 73K212 modem expects
Bell 212A's 2225 Hz answer tone. Closing that needed a patch to datamodem
(U2, in a fork of it), plus an interim per-terminal MODEM table and a
2225 Hz bootstrap route for each terminal (DS-5). Nothing had been built.

[`jxburros/mm_manager`](https://github.com/jxburros/mm_manager), another
fork of `hharte/mm_manager`, already contains a soft modem built into
mm_manager, in commit
[`4182011`](https://github.com/jxburros/mm_manager/commit/41820114b388256b8f64fcbd624870bd5cc23d71)
(2026-10-19, MIT):

| File | What it does |
|---|---|
| `src/mm_softmodem.c` | Bell 212A modem, answer and originate: 1200 bit/s DPSK, 1+x^-14+x^-17 scrambler, Gardner timing recovery, async 8N1 framing, G.711 codecs. **Answers with 2225 Hz** (2100 Hz optional). |
| `src/mm_vmodem.c` | Hayes modem emulation behind mm_manager's serial layer (`AT`, `RING`, `CONNECT`, `NO CARRIER`, DCD, DTR hang-up), so `mm_proto.c` is unchanged. |
| `src/mm_sip.c` | Minimal SIP user agent (REGISTER with digest auth, INVITE/ACK/BYE/CANCEL/OPTIONS/UPDATE) and RTP with a re-ordering buffer. |
| `src/mm_osal.c` | Sockets, threads and time for Linux, macOS and Windows. |
| `tests/test_softmodem.c`, `tests/test_sip_vmodem.c` | Modem loopback over a simulated channel (G.711, noise, echo, ±200 ppm clock offset) and an end-to-end SIP call, in ctest. |

Its README states that it **has not been tested against a real Millennium
terminal**. Its author reports one-off interoperation with spandsp's V.22
modem at 1200 bit/s; that check is not in its CI.

## Decision

Import that commit's soft modem, SIP client and portability layer into
this fork, with attribution, and make it the soft modem path. Do not import
the rest of that fork (control panel, Protel/Elcotel, release tooling,
branding).

Owner decisions recorded with this ADR (2026-11-15):

| ID | Question (from ADR-0002) | Answer |
|---|---|---|
| DS-1 | Accept a GPL component at a process boundary? | Yes, but moot: nothing GPL is used on this path. |
| DS-2 | Shim language? | C. Moot: there is no shim; the modem is C inside mm_manager. |
| DS-3 | Pursue U2 (Bell answer tone) in datamodem? | Not needed: the imported modem answers with 2225 Hz. |
| DS-4 | el-modem as a test fallback? | Yes, as ADR-0002 says, if the built-in modem fails on hardware. |
| DS-5 | Per-terminal MODEM table "answer tone not required"? | Yes, as ADR-0002 says, but only as a fallback if a terminal will not train on the 2225 Hz answer. |

## Changes made on import

1. **Network exposure (card data, SECURITY_RULES 4 and 5 in spirit; SPEC C-1, C-6).**
   Upstream of the import, the SIP and RTP sockets bound `INADDR_ANY` and
   accepted SIP and RTP from any address (RTP latched to whatever sent
   first). Now:
   - `bind=<ipv4>` sets the address both sockets bind to. Default
     `127.0.0.1`. `0.0.0.0` and host names are refused.
   - Only one peer is accepted: `allow=<ipv4>`, or by default the
     registrar's address. Every other SIP or RTP packet is dropped and
     counted on stderr. `listen` mode on a non-loopback address requires
     `allow`.
   - `local_ip` (the address advertised in Contact and SDP) defaults to
     `bind`. This disables the imported NAT discovery, which this setup
     does not need (Asterisk on the same host or on the voice VLAN).
   - RTP is also accepted from the media address named in SDP from the
     allowed SIP peer (Asterisk `media_address`, several interfaces).
   - A registrar on another host with the default loopback bind is refused
     at start-up (it could never register).
2. The `-S` parse errors no longer echo the argument or option text, which
   can contain the SIP password.
3. Line data is wiped from memory (SECURITY_RULES 3): each byte in the
   modem's FIFOs is zeroed as it is read, FIFOs are zeroed when cleared,
   and the modem, virtual modem and SIP agent structures (bit buffers, RTP
   audio slots) are zeroed before they are freed.
4. Upstream files touched only at call sites: `mm_serial.[ch]` and
   `mm_connection.c` (as imported), `mm_manager.h` (one field),
   `mm_manager.c` (`-S` option and help). No branding changes.
5. New test `tests/test_sip_access.c` (ctest `sip_access`): the bind/allow
   parse rules, and that OPTIONS from a non-allowed loopback address is
   dropped while the allowed one is answered.

## Verification (2026-11-15)

- Debian bookworm, gcc, `-Wall -Wextra -Werror`, all targets: clean.
  `ctest`: `mm_cardman_test`, `softmodem`, `sip_vmodem`, `sip_access` pass.
- Leak check (rule 9): `test_sip_vmodem` was run once with the terminal
  sending test PAN `4012888888881881` (ASCII, packed BCD, nibble-swapped
  BCD) and dummy PIN `1234`, at SIP debug level 2 (every SIP message
  printed). The whole stdout/stderr, decoded for ASCII, BCD, hex text and
  one-hex-byte-per-line forms: no match. The imported modem and SIP code
  print states, levels and SIP messages only, never line bytes.
- macOS (clang, `-Wall -Wextra -Werror`): the three modem/SIP tests
  compiled directly and pass; the allowed-peer half of `sip_access` skips
  there (only 127.0.0.1 exists). The full CMake build was not run on macOS.
- Not verified: Windows (CI will build it); anything against a real
  terminal or a real Asterisk.

## Consequences

- No datamodem, no shim, no extra Unix user, socket or PTY. Card data on
  the line exists in clear only in RTP audio and in mm_manager's memory.
- New sink to add to `THREAT_MODEL.md`: **RTP audio between Asterisk and
  mm_manager** (modulated PAN/PIN, decodable). Control: loopback or an
  isolated voice VLAN, `bind`/`allow`, and no recording on the route
  (SPEC C-1, C-3 still apply).
- Anything that reaches the modem can act as a terminal (ADR-0001 A-1).
  `allow` limits that to the PBX; the dialplan must limit it to the
  payphone ATA's context (SPEC C-6).
- mm_manager handles one call at a time, as before. Run one instance per
  simultaneous call-in, each with its own SIP user, `port`, `rtp_port` and
  working directory.
- We now own about 3,300 lines of imported C, including DSP. Bug fixes
  from `jxburros/mm_manager` can be cherry-picked; the files keep their
  original headers.
- `-v`, `-l`, `-p`, `-u` still expose card data exactly as with a hardware
  modem (rule 4, task T4). The SIP path adds no new raw-byte output.

## Hardware checks (replace SPEC §9.2 SM-1 to SM-3)

| # | Test | Pass when | If it fails |
|---|---|---|---|
| SIP-1 | Coin or craft call-in through PhreakNet to `mm_manager -S`, default 2225 Hz answer, default MODEM table. 10 call-ins. | 10/10 reach `CONNECT` and finish the session | Check the ATA settings and `core show channel` (both legs `ulaw`). Then try `answer_tone=2100`, then the DS-5 MODEM table. |
| SIP-2 | A real hardware modem set to Bell 212A (`AT+MS=B212`) dials the same number from another ATA port, before any payphone test. | Trains; `mm_manager` logs `Connected!` | Modem or SIP problem, not the terminal's. |
| SIP-3 | Full install session (all tables). | Completes, within 25% of a hardware-modem baseline | Jitter or loss on the path. |
| SIP-4 | Terminal hangs up mid-download; mm_manager killed mid-call. | Next call-in works | Call-state bug in the imported vmodem/SIP code. |
| SIP-5 | Card call end to end (needs test cards, `docs/wapi-bridge/SPEC.md` §14.1). | As `HARDWARE_TEST_PLAN.md` 3.1 | Compare with the hardware modem on the same day. |
| SIP-6 | Leak audit after SIP-5 over the host's logs and Asterisk logs (rule 9). | No match | Blocker. |

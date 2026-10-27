# ADR-0002: Soft modem for terminal dial-in (datamodem)

- **Status**: Proposed, 2026-10-26. Owner decisions DS-1 to DS-5 open.
- **Scope**: the modem leg between a Nortel Millennium terminal and
  mm_manager, on the payphone host. No change to `src/`; no change to the
  card path or to the decisions in `AI/CARDMAN_CONTEXT.md` or ADR-0001.
- **Detail**: [`SPEC.md`](SPEC.md) (integration spec: topology, the DTE
  shim contract, configuration, security controls, tests, phasing).

## Context

- The terminal always dials the manager; the manager never calls out
  (`README.md`, "Overview"). The modulation is Bell 212A, 1200 bit/s
  full duplex: mm_manager's default init string asks the hardware modem
  for it with `+MS=B212` (`DEFAULT_MODEM_INIT_STRING`,
  `src/mm_manager.c` L261).
- mm_manager talks to its modem only through a serial device and a
  small Hayes subset (`src/mm_modem.c`): it sends `ATZ` and the init
  string and needs `OK`, waits for `RING`/`CONNECT`/`NO CARRIER`
  (`modem_responses[]`, substring match), hangs up by dropping DTR
  (`hangup_modem()`, `USE_MODEM_DTR`), and checks DCD/RI between bytes
  unless run with `-w` (`receive_mm_packet()`, `src/mm_proto.c`).
- Today that modem is hardware. The README warns that Conexant soft
  modems never had an open Linux driver and recommends a "real" modem
  (`README.md`, "Items Needed" item 5). The owner already routes the
  manager number through Asterisk (`README.md`: "I intercept in the
  Asterisk dialplan and send to the modem").
- `COMPATIBILITY_PLAN.md` §1 names the soft modem path as
  `Asterisk app_softmodem -> TCP -> socat PTY` (integration-repo SOW
  Phase 3) without having evaluated it, and §5 lists "`app_softmodem`
  cannot demodulate this terminal" as a risk.
- Every byte on this leg is card data: the full PAN and PIN of every
  card call cross it in the auth request (`AI/SECURITY_RULES.md` rule 4
  treats every raw-byte sink of this stream as a card-data leak).
- Owner requirement for this ADR: an external, well-documented soft
  modem with a fully open licence that fits this architecture, chosen
  by comparison against verifiable facts rather than taking the first
  one found.

## Selection criteria

Gates (a candidate that fails one is out):

- **G1 Open, all the way down.** An OSI-approved licence on the project
  *and* everything it links; no binary-only DSP, firmware or object
  files.
- **G2 Modulation.** Answers at 1200 bit/s DPSK (V.22 / Bell 212A
  family). FSK-only modems (Bell 103/202, V.21, V.23) cannot carry
  this terminal.
- **G3 Deployable here.** Runs headless and unattended, 24x7, on Linux
  (including arm64, since the README recommends a Raspberry Pi), and
  answers an inbound VoIP call.

Ranked criteria (for candidates that pass every gate):

- **R1 Interface fit**: how close its data interface comes to a serial
  line with a Hayes subset, and how much glue or fork delta it needs.
- **R2 Licence fit**: no licence conflict with the processes it has to
  be loaded into or linked with (mm_manager is MIT; Asterisk is GPLv2).
- **R3 Card-data surface**: which processes end up holding demodulated
  PAN/PIN, and whether its diagnostics can be kept from writing them.
- **R4 Evidence it works**: tests, CI, releases, interop with real
  modems over real VoIP paths.
- **R5 Documentation**: enough to configure, debug and audit it without
  reading all the code.
- **R6 Maintenance**: recent activity and a way to send changes upstream.

## Evidence

Each claim below was checked on 2026-10-26 against the project's own
source at the commit given (shallow clones, read only, nothing built or
run) or against the primary page cited. File:line references point
into the named upstream repository at that commit. Star counts and dates
come from the GitHub API on the same day.

### Bell 212A versus V.22: the one fact every candidate is measured on

- Bell modes answer with a **2225 Hz** tone; V.25/V.22 answer with
  **2100 Hz** ([Answer tone](https://en.wikipedia.org/wiki/Answer_tone):
  "2100 Hz (or 2225 Hz for Bell modes)"). A modem chip datasheet says
  the same: in Bell 212A mode the TDK/Teridian 73K222AL "employs a 2225
  Hz answer tone"; in V.22 mode it "recognizes and generates a 2100 Hz
  answer tone"
  ([73K222AL datasheet](https://pdf.jiepei.com/73k222al-9204644.html)).
- Both are 1200 bit/s DPSK, and the 212A answerer needs no guard tone
  (spandsp `src/spandsp/v22bis.h` L64: "No guard tone is the norm is
  some areas, such a North America" [sic]).
- Wikipedia calls V.22 "equivalent, but incompatible" with Bell 212A
  ([Bell 212A](https://en.wikipedia.org/wiki/Bell_212A)) and gives no
  detail. Whether a Millennium's 212A originate side accepts a V.22
  answerer is therefore **unknown and can only be settled on hardware**.

### The Millennium's own modem (cross-reference, 2026-10-26)

Sources: Nortel manuals in
[muccc/millennium](https://github.com/muccc/millennium) @ `a70a19e`
(`documentation/`); upstream issues on
[hharte/mm_manager](https://github.com/hharte/mm_manager/issues); and the
TDK/Teridian 73K222AL datasheet (Rev 6.0, 2005, the closest published
sibling of the terminal's 73K212). Text was extracted from the PDFs with
a stdlib script; nothing was run from those repositories.

- **Modulation is Bell 212A only.** "The modem signals using Bell 212A
  1200 bps Differential Phase Shift Keying (DPSK) standard protocol"
  (Nortel *Millennium desk terminal general specification* GS5U10,
  issue 00.02, Feb 1997, "Modem signaling").
- **Two different modem chips ship on control boards.** An owner
  reported a **73K324L-28IH** on the surface-mount board and a
  **73K212ASL-IP** on the "rev01" board. A Rockwell modem (Supra 288,
  `ATB1`) handshook with the first and "Won't handshake at all" with the
  second, while a Conexant (Lenovo 56K) modem worked with both
  ([hharte/mm_manager#3](https://github.com/hharte/mm_manager/issues/3)).
  The upstream maintainer replied that he had not known the two board
  revisions behaved differently.
- **What those chips can hear.** Per the 73K222AL datasheet, it "is an
  enhancement of the 73K212L/AL single-chip modem which adds V.22 and
  V.21 modes to the Bell 212A and 103 operation of the 73K212AL". The
  family's detect register says "Answer Tone Detect: Indicates
  detection of 2225 Hz answer tone in Bell mode or 2100 Hz in CCITT
  mode", in originate mode. The 73K324L lists "V.22bis, V.22, V.21, V.23
  and Bell 212A" ([73K324L datasheet](https://pdf.jiepei.com/73k324l-16665277.html)).
  **So a 73K212-based terminal has no CCITT mode and cannot detect
  datamodem's 2100 Hz tone at all.** A 73K324L terminal could only if
  its firmware puts the chip in CCITT mode, which a Bell-only spec
  makes unlikely. The chips also report "Unscrambled Mark Detect" (more
  than 165.5 ms of unscrambled marks) and "Carrier Detect", and the
  host microcontroller runs the handshake from these detector bits.
- **The terminal's firmware lets the manager turn off the answer-tone
  requirement.** The MODEM table (DLOG `0x21`, "Modem Configuration
  Parameters") in Nortel's *Millennium Database Design Report MSR 2.1*
  (`documentation/manager/A0xxxxxx_00_02.pdf`, pp. 2-279 to 2-289) has
  these fields:
  - **ENABLE ANSWER TONE DETECT**: default `1`; `0` = "No detection of
    (or looking for) answer tone";
  - **ANSWER TONE NOT REQUIRED**: default `0`; `1` = "Yes - answer tone
    is NOT required (use carrier only)". It "Indicates whether an answer
    tone is required before proceeding with a data call";
  - answer-tone and carrier-mark debounce counts, **DELAY AFTER CARRIER
    MARK DET** (default 500, in 10 ms units, so 5 s), and ENABLE CARRIER
    LOST TIMEOUT.
- **mm_manager already sends this table.** `DLOG_MT_MODEM_PARMS` is in
  its download lists (`src/mm_manager.c` L72, L118, L163, L196, L229),
  from `tables/<terminal_id>/mm_table_21.bin`, or else
  `tables/default/mm_table_21.bin` (L1426-1429). The 35-byte default
  file decodes field by field against the report's field order, and
  every value equals the documented default except CARRIER LOST DEB OFF
  CNT (`0x64` = 100 against a documented 3). Its byte 33, the
  answer-tone config bits, is **`0x02`**: detection enabled, answer tone
  **required**. The bit order (bit 0 spare, bit 1 enable detect, bit 2
  not required) is inferred from the report's field order and the
  defaults. It has not been tested on hardware.
- **Answer supervision is a precondition for any VoIP line.** The
  terminal needs polarity reversal on answer; without it, a Conexant
  modem behind a Grandstream HT802 saw "Carrier lost" and the terminal
  showed Error 51 until polarity reversal was enabled
  ([hharte/mm_manager#25](https://github.com/hharte/mm_manager/issues/25);
  upstream `README.md`, "Phone Line").

**What this means for the decision:**

- Under the default MODEM table, datamodem's V.22 answer is expected to
  **fail** on a 73K212 board: answer tone is required, and that chip
  cannot detect 2100 Hz. It may fail on a 73K324L board too.
- Sending a MODEM table with ANSWER TONE NOT REQUIRED (byte 33 = `0x06`)
  should let the terminal proceed on carrier alone. That is a
  terminal-side table change, not a modem change.
- **Bootstrap problem:** the terminal applies a new MODEM table only
  after a successful call-in has downloaded it. Its settings before the
  first download are unknown, and probably the documented defaults. So
  the first call-in to a factory-fresh terminal may still need a 2225
  Hz answerer: a hardware modem, or el-modem.
- What happens after carrier is detected with a V.22 (rather than Bell)
  answerer is still unknown. The terminal's firmware decides from the
  unscrambled-mark and carrier bits, and only hardware will show it.

**Finding (first pass): no candidate that passes G1 sends the Bell
2225 Hz answer tone at 1200 bit/s.** The third pass (below) found two
projects that do: el-modem, which is open but experimental, and
cryan209/v90modem, which has no licence. Upstream spandsp declares `V22BIS_BELL212A_COMPATIBILITY_MODE`
(`src/spandsp/v22bis.h` L72) but nothing in `src/v22bis_tx.c` or
`src/v22bis_rx.c` reads it (the only other reference is the header
itself), although spandsp can generate the Bell tone
(`MODEM_CONNECT_TONES_BELL_ANS`, `src/modem_connect_tones.c` L373-375).
Every candidate below that does V.22 sends 2100 Hz. This gap is common
to all of them, so it does not separate them, but it does shape the
decision (S4).

### Candidates

| # | Project @ commit | Licence (verified in) | G1 open | G2 1200 DPSK answer | G3 headless Linux, VoIP in | Interface to mm_manager | Verdict |
|---|---|---|---|---|---|---|---|
| 1 | [billsimon/datamodem](https://github.com/billsimon/datamodem) @ `1aa248a` (2026-10-14), release v1.1.0 | GPL-3.0-or-later (`LICENSE`; README "Licence"). Release binaries link pjproject (GPLv2+), spandsp (LGPL-2.1), OpenSSL (Apache-2.0); source tarballs attached to releases | Pass | Pass: `--modulation v22` (README "Modulations" table); answer tone 2100 Hz only (`src/config.c` L93-94; Bell tone only for Bell 103, `src/modem.c` L604-606) | Pass: own SIP stack, `datamodem answer`; static binaries for Linux x86_64 and arm64 | `answer --exec`: line as the command's stdin/stdout, stdin EOF and SIGHUP on caller hang-up, call cleared when the command exits; one call at a time, others get 486 (README "Answering for a BBS") | **Selected** |
| 2 | [billsimon/asterisk-Softmodem](https://github.com/billsimon/asterisk-Softmodem) @ `4616d12` (2026-10-13) | `app_softmodem.c` header "GNU General Public License" (no version); vendored engine is datamodem's, `engine/LICENSE` GPLv3 | Pass | Pass, same engine (`v(V22)`, `f`) | Pass, but only inside Asterisk | Dialplan `Softmodem(host,port,opts)` opens a **TCP client** to a server (README "Dialplan"); needs a Hayes adapter in front of mm_manager | Runner-up (rejected: R2, R3) |
| 3 | [proquar/asterisk-Softmodem](https://github.com/proquar/asterisk-Softmodem) @ `14e5b4f` (2016-05-25) | Header "GNU General Public License", no version, no licence file | Pass | Weak: `v22bis_init(NULL, 1200, 0, ...)` with no options (`app_softmodem.c` L517); V.22bis added "compiles fine but is still untested" (commit `edf1ab9`) | Inside Asterisk | TCP client, as #2 | Rejected: superseded by #2, unmaintained since 2016 |
| 4 | [gnomsa/sip-softmodem](https://github.com/gnomsa/sip-softmodem) @ `ce52a8e` (2026-09-02) | MIT (`LICENSE`) | Pass | Pass on paper: V.22 1200 (README); 2100 Hz answer tone | Pass, Linux only | **PTY with an AT parser**: `RING` on incoming SIP, `ATS0`/`ATA` (README "Current scope") | Rejected: R4, R5, and two hard incompatibilities (below) |
| 5 | [CasualArclamp/BinModem](https://github.com/CasualArclamp/BinModem) @ `8e16aee` (2026-09-27) | GPL-3.0 (`LICENSE`) | Pass | Pass: V.22 1200 (README table) | **Fail**: built for Windows (`x86_64-pc-windows-msvc`), sound-card line, com0com virtual COM port, GUI (`docs/design/architecture.md` "Decisions"); SIP has no listener (`docs/design/sip.md` L881) | Virtual COM port on Windows | Rejected at G3 |
| 6 | [freeswitch/spandsp](https://github.com/freeswitch/spandsp) @ `8f1e164` (v3.1.1, 2026-07-31) | LGPL-2.1 library, GPL-2 tests (`COPYING`) | Pass | Has a V.22bis pump and an AT interpreter (`src/at_interpreter.c`, `src/data_modems.c`); Bell 212A flag unused (above) | A library: no SIP, no daemon | None: we would have to write and maintain the modem host | Rejected as a direct integration; used indirectly through #1 |
| 7 | [AonCyberLabs/D-Modem](https://github.com/AonCyberLabs/D-Modem) @ `636959b` (2021-10-29) | GPLv2 (`COPYING`) for the wrapper | **Fail**: `slmodemd/dsplibs.o` is "ELF 32-bit LSB relocatable, Intel 80386" (`file` output), a binary-only DSP; Ubuntu's sl-modem notes say "this package is NOT FREE SOFTWARE" ([Ubuntu wiki](https://wiki.ubuntu.com/MainInclusionReportSlModem)) | (true Hayes modem on a pty) | i386 only, so no arm64 | Would be ideal (real AT modem on a pty) if it were open | Rejected at G1 |
| 8 | [szeremeta1/slmbridge](https://github.com/szeremeta1/slmbridge) @ `708dc62` (2026-09-13) | MIT for the bridge | **Fail**: runs `slmodemd`, the same blob as #7 (README) | — | — | — | Rejected at G1 |
| 9 | [kamalmostafa/minimodem](https://github.com/kamalmostafa/minimodem) @ `bb2f34c` (2021-07-17) | GPL-3 (`COPYING`) | Pass | **Fail**: FSK only, "Bell103, Bell202, RTTY, TTY/TDD" (README) | — | — | Rejected at G2 |
| 10 | [linmodem](https://bellard.org/linmodem/) 0.2.5 | No licence on the project page | Unverified | **Fail**: V.21, V.23, partial V.34; "It does nothing useful yet"; page last updated Mar 6, 2000 | — | — | Rejected at G1/G2 |
| 11 | [go4retro/tcpser](https://github.com/go4retro/tcpser) @ `fe7feff` (2023-11-24) | GPL-2.0-or-later (README "License") | Pass | Not a modem: emulates a Hayes modem over TCP | Pass | Considered only as **glue** for #1/#2 (TCP in, `RING`/`CONNECT` on a serial port) | Rejected as glue: listens on `INADDR_ANY` (`src/ip.c` L70) and has payload tracing (`-t`), both wrong for a card-data stream |

A second GitHub search limited to C++ (2026-10-26) found nothing that
passes G2. The search ran `gh search repos` over softmodem, soft or
software modem, V.22, V.22bis, Bell 212, V.32, V.34, V.90, Hayes, DPSK,
spandsp, SIP or VoIP modem, and dial-up, plus a code search for
`v22bis` and `Bell 212`. The only relevant hit is:

| # | Project @ commit | Licence | Why it is out |
|---|---|---|---|
| 12 | t38modem ([SourceForge](https://sourceforge.net/projects/t38modem/), v2.0.0, page updated 2017-01-12; build bundle [michaaa/t38modem](https://github.com/michaaa/t38modem) @ `e4bb0e4`) | MPL-1.0 (file headers, e.g. `t38modem/pmodeme.cxx`) | Best interface fit of all: a C++/OPAL SIP/H.323 endpoint that shows applications a Hayes modem on a PTY. But it is a **fax/voice** modem only: `ATBn` (V.22/Bell 212A) and `AT+FCLASS=0` (data) are stubs that accept the command and do nothing (`pmodeme.cxx` L300, L2266-2270). Fails G2. Its PTY + AT front end is what U1 would give datamodem. |

Evaluated on request (2026-10-26):

| # | Project @ commit | Licence | Why it is out |
|---|---|---|---|
| 13 | [kulp/tynsel](https://github.com/kulp/tynsel) @ `f360ed6` (2026-04-01; C, 534 commits since 2012, 11 stars, one main author) | MIT (`LICENSE`) | Fails G2: "300-baud FSK using the Bell 103 dataset" only (README; description "A tiny Bell 103 (300 baud FSK) softmodem"), with no PSK code. Fails G3: `gen` and `listen` are one-way filters between ASCII and raw 16-bit 8 kHz audio, with no answer handshake, no AT interface and no SIP/RTP. The AVR build is described as "non-working". The README itself says "look at minimodem instead". Well made for what it is: built for an ATtiny412 (under 256 bytes of RAM), with CI that builds with `WERROR=1` and runs an encode/decode round trip (`.github/workflows/c.yml`). |

The other C++ hits are FSK-only or V.23-only (aquila12/v23,
m4ce-w1ndu/software-modem, the Arduino `SoftModem` family), radio data
modems (modem73, fldigi), byte-level modem *emulators* with no audio
(XBAND emulator, ESP8266/ESP32 Hayes WiFi modems, `at-modem-server`),
emulators of old machines that model a modem chip (MAME, Altirra), or
empty skeletons (Jonathan-Qian/soft-modem).

### Third pass (2026-10-26)

A wider search covered all languages, with `gh search repos` over about
20 softmodem, SIP/VoIP modem, V.22bis, Bell 212 and dial-up terms (116
unique repositories). It also ran code searches for `BELL212A`,
`bell_212a`, `Bell 212A answer`, `2225 Hz answer tone`,
`MODEM_CONNECT_TONES_BELL_ANS`, `V22BIS_BELL212A_COMPATIBILITY_MODE` and
`+MS=B212`. Everything relevant was cloned and read; nothing was built
or run.

| # | Project @ commit | Licence | G1 | G2 | G3 / interface | Verdict |
|---|---|---|---|---|---|---|
| 14 | [donfanning/el-modem](https://github.com/donfanning/el-modem) @ `391b216` (first commit 2026-10-13; 105 commits, one author) | GPL-3.0-or-later (`LICENSE`; `docs/SOURCE_PROVENANCE.md`) | **Pass, with a flag**: no binary DSP (the SmartLink blob is only an interop peer, "not included"). The author records that the V.32 code "was developed with Fisher's implementation in view", and does not treat Rossi's MIT notice as covering Fisher's files (`docs/SOURCE_PROVENANCE.md`). That code is not on our V.22 path, but it ships in the same program. Separate commercial licensing is "to be finalized" (`docs/REDISTRIBUTION.md`). | **Pass, and the only open project with a Bell answer**: `AT+MS=B212` selects a `bell212a` profile that "emits/detects the 2225 Hz answer mark" (`lab/asterisk/audio-peer/v22.py` L30; parser `lab/asterisk/modem_dce.py` L3184-3203, mapping L3464-3468). Its own docs call the profile "preliminary", with "internal" evidence (`docs/AT_COMMANDS.md` L122-127). Pure Python + NumPy DSP; spandsp is only a lab reference. | **Native fit**: a Hayes modem on a PTY (`--serial-backend pty`, `--link`), `RING` on incoming SIP, `S0` auto-answer (`modem_dce.py` L2256-2262), SIP via S-registers. No shim needed. mm_manager's default init string returns `ERROR` at `E=1` (`modem_dce.py` L2929-2931: `E` must be followed by a digit), so it would need `-i "ATS0=1S7=3&D2+MS=B212,0"` (not run). **Limits**: Linux releases are x86-64 only, needing GLIBC 2.38+ (`docs/RUNTIME_REQUIREMENTS.md` L12), so no Raspberry Pi. Timing was qualified only with the CPU governor set to `performance`; a baseline run "failed timing gates on six calls" (README). "Experimental ... source preview, not a stable release" (README). **Call recording is on by default**: `EL_MODEM_RECORDING` defaults to `1` (`lab/asterisk/el_modem.py` L104-105), and the docs say "Full PCM/DSP recording is enabled by default" (`docs/DESKTOP.md` L106), which would write every PAN and PIN to disk as decodable audio. | **Designated alternate for the Bell gate** (S4). Not primary: maturity, no arm64, and a recording default that fails open for card data. |
| 15 | [cryan209/v90modem](https://github.com/cryan209/v90modem) @ `415368a` (2026-10-27; first commit 2026-03-02) | **None** at the top level (GitHub reports no licence; no `LICENSE`/`COPYING` outside bundled pjproject and spandsp) | **Fail**: with no licence, its own code is all rights reserved. It also ships `hsf_rom_patched.bin`, a patched Conexant HSF modem ROM, and a copy of ITU software with `.exe` tools. | Pass, and the most complete: `AT+MS=B212` / `--mode b212`, a 2225 Hz answer, the V.22 scrambler, no guard tone, and Bell 103 fallback (`docs/bell212a.md`). It **implements spandsp's dead `V22BIS_BELL212A_COMPATIBILITY_MODE`** in its copy of `spandsp-master/src/v22bis_tx.c` and `v22bis_rx.c` (e.g. `v22bis_rx.c` L689, L758-762, L1205), with tests (`bell212a_test.c`). | SIP/G.711, answers, PTY (`CLAUDE.md`) | Rejected at G1. **But** its spandsp changes keep spandsp's LGPL-2.1 headers (`v22bis_tx.c` L1-30), and modified LGPL files must stay LGPL when distributed. That makes them a usable reference implementation for U2, which is GPL-3-compatible. Asking the author to add a licence would make the whole project worth re-evaluating. |
| 16 | [cyberbalsa/voipmodem](https://github.com/cyberbalsa/voipmodem) @ `ecd6f14` (2026-06-03; 7 commits) | GPL (D-Modem fork) | **Fail**: `slmodemd/Makefile` L35 links `dsplibs.o`, the same closed i386 blob as #7. The blob is absent from the repo only because `.gitignore` L21 ignores `*.o`, so a fresh clone cannot build without fetching it. The README's "pure-software modem DSP" wording is inaccurate. | slmodem's DSP | Hayes on `/dev/ttySL0`, inbound auto-answer (a real improvement over D-Modem); hard-wired to voip.ms in its docs; `slmodemd` is "32-bit (i386)" (README "Build") | Rejected at G1 (and arm64) |
| 17 | [philpem/slmodem_dsp_re](https://github.com/philpem/slmodem_dsp_re) @ `e669052` (1044 commits since 2026-08-04) | None (no licence file) | **Fail**: a "source reconstruction of `dsplibs.o`" derived from the binary (README), which ships the original blob under `ref/`, and has no licence of its own | Would cover Bell 103 to V.92 if finished | A library reconstruction, no daemon | Rejected at G1. If it ever gets a licence, it would make slmodem-based tools (#7, #8, #16) open. |
| 18 | [randyrossi/fisher-modem](https://github.com/randyrossi/fisher-modem) @ `3fb915c` (2020-04-03) | MIT notice (Rossi); Fisher's original carries no explicit grant (per el-modem's provenance note) | Unclear | **Fail**: no V.22 or Bell 212 anywhere in the tree (search returned 0 files); commits built `.o` files | Sound card / Bluetooth | Rejected at G2 |
| 19 | [Casandro/btx_modem_sip](https://github.com/Casandro/btx_modem_sip) @ `f57c5a2` (2026-08-09; 2 commits) | Unlicense | Pass | V.23/V.22/V.22bis autodetect (description), but built as a Bildschirmtext **exchange** for FTZ 157 D2 layer 2 (README) | Purpose-built; self-described "slopware" | Rejected: wrong purpose, no maturity |

Also seen and ruled out on their README or licence alone:
nickvsnetworking/softmodem (no licence);
ooper1234/V90AnswererSoftModem (GPL-3.0, the V.90 digital answerer, and
its V.22 notes list "Annex A (Bell 212A compat) timing" as not done,
`docs/v22bis.md` L163); williameggers/softmodem and sipmodem (2015-2017,
inactive); Ankeraout/softmodem and pysipmodem (Python, no licence);
SuperInstance/modem-core and chrissnell/omnimodem (no Bell 212A);
george-hopkins/hsfmodem and agrsm (repackaged proprietary Conexant and
LSI drivers); iaxmodem (fax only); jtarrio/SoftwareModem (V.21 only);
and the radio modems (modem73, fldigi, soundmodem, VARA clients).

Notes on the close calls:

- **#2 versus #1 (same DSP, different packaging).** Asterisk's licence
  says "the GPL applies to all loadable Asterisk modules used on your
  system" and that Asterisk is GPL version 2
  ([Asterisk LICENSE](https://github.com/asterisk/asterisk/blob/master/LICENSE)).
  The module links GPLv3 code (`engine/LICENSE`). The FSF lists GPLv2-only
  and GPLv3 as incompatible
  ([GPL FAQ](https://www.gnu.org/licenses/gpl-faq.html#v2v3Compatibility)),
  so a built module is fine to run privately but not clean to hand to
  anyone else, and that fails the "fully open" spirit of the request
  (R2). It would also put demodulated PAN/PIN inside the PBX process and
  within reach of Asterisk's own debug logging (R3). It also ties the
  build to an Asterisk version: CI builds a separate `.so` for each
  release of 20, 22 and 23. Standalone datamodem has none of these
  problems and runs the same engine (`engine/UPSTREAM`: datamodem
  `e947e7d`, synced 2026-10-12).
- **#4 is the best interface fit and the only permissive licence, but
  not usable yet.** Its AT parser accepts only `D A H O E0/1 V Q Z I
  S0 +MS` (`src/at.c` L13). It answers `ERROR` to mm_manager's default
  init string (`ATE=1 S0=1 S7=3 &D2 +MS=B212`: `E=`, `S7=`, `&D` all
  fall through to `ok=0`). `-i` could work around that. What `-i` cannot
  fix is that its SDP offers **PCMA only** (README), while North
  American ATAs and trunks run μ-law, so every call would need an
  A-law/μ-law transcode in the audio path. It is also young: all 140
  commits fall between 2026-08-28 and 2026-09-02, with no activity
  since, one author, no CI workflow in the repository, and a
  README-only manual. Its V.22 pump is `src/v22.c` (20 lines) plus
  `src/v22_handshake.c` (38 lines), with no interop record against real
  V.22 hardware. Worth re-checking once it supports μ-law and shows
  sustained activity.
- **Why datamodem's evidence is stronger.** Every release build runs
  the test suite (`.github/workflows/release.yml` "Test" step);
  `datamodem selftest --modulation v22` loops two modems back to back
  per modulation, with impairments (DEVNOTES "Testing"). V.34, V.32bis
  and V.32 have been used against "a Cisco MICA and BBSes" (README
  "Modulations"). It documents a real spandsp defect and works around
  it: the 2012 `spandsp-0.0.6` tarball's V.22bis "does not carry
  data ... delivers a constant `0x55`", so it vendors a mid-2014 V.22bis
  (DEVNOTES "The V.22bis situation"). That defect bears directly on
  `COMPATIBILITY_PLAN.md`'s `app_softmodem` risk, because proquar's
  module (#3) calls the stock library.
- **Weak points of datamodem, stated plainly.** One maintainer, created
  2026-10-02, 0 stars, three release tags in two weeks (v1.0.0 to
  v1.1.0).
  V.22 at 1200 has self-test coverage but no published interop record
  against a Bell 212A originator. It sends no Bell answer tone. These
  are the reasons for S4 and S6.

## Decisions

**S1. Use datamodem, as a standalone process.** It is the only
candidate that passes G1-G3 and also leads on R2-R6. It runs as its
own SIP endpoint registered to the owner's existing Asterisk (not as
an Asterisk module). It answers at V.22 1200 bit/s, 8N1, with no
stepping down and V.42 off: the Millennium is a plain async 212A
originator, so V.42 detection would gain nothing and only adds a
detection phase before data flows (datamodem README "When it goes
wrong" notes the DC1 characters a detecting caller sends).
*Rejected*: app_softmodem (R2, R3, version coupling; see Evidence);
gnomsa/sip-softmodem (A-law only, maturity); D-Modem/slmbridge (binary
DSP); writing our own host on spandsp (we would own a DSP stack).

**S2. Join datamodem to mm_manager with a fork-side DTE shim, and change
nothing in `src/`.** `mm-softmodem-dte` is a new MIT tool under `tools/`.
It is a persistent process. Toward mm_manager it presents a PTY that
behaves like a Hayes modem: `OK` to `AT` lines, `RING` and
`CONNECT 1200` when a call arrives, `NO CARRIER` when it ends. Toward
datamodem it accepts one connection per call on a Unix-domain socket,
opened by the shim's own `connect` subcommand, which datamodem runs as
its `--exec` command. mm_manager runs unchanged with `-m -w` (`-w`
because a PTY has no DCD; its default init strings get `OK`). Closing the per-call connection is the
hang-up, because datamodem clears a call when its `--exec` command
exits. Contract in `SPEC.md` §4.
*Why*: it keeps the rebasable upstream delta at zero, as
`COMPATIBILITY_PLAN.md` design rule 4 requires, and it puts the only
new code that handles card bytes in this repo, where `SECURITY_RULES.md`
can be enforced and reviewed.
*Rejected*: tcpser as glue (listens on all interfaces, payload tracing);
`socat` PTY plus TCP (no Hayes responses, so `init_modem()` fails); a new
TCP transport inside mm_manager (an upstream-file delta for a problem a
small standalone tool solves).

**S3. A process boundary is the licence boundary.** datamodem (GPL-3.0+)
and mm_manager (MIT) share no address space. They exchange bytes over a
pipe, a Unix socket and a PTY, so neither licence reaches the other.
This repo does not vendor, build, or redistribute datamodem, and CI
does not download it. Operators install a pinned upstream release and
record its SHA-256 in their deployment notes (`SPEC.md` §8).

**S4. Bell 212A compatibility is a hardware gate, and the hardware
modem stays supported.** Before any reliance on the soft modem, the
hardware session first records which modem chip the terminal has (SM-0:
73K212 or 73K324L). It then runs the answer-tone matrix in `SPEC.md`
§9.2:

- SM-1: datamodem's default 2100 Hz tone with the default MODEM table
  (expected to fail on a 73K212; recorded as the baseline);
- SM-1a: prove the MODEM table bit on its own, using a hardware modem
  with its answer tone suppressed;
- SM-1b: the same, after a call-in has downloaded a per-terminal MODEM
  table with ANSWER TONE NOT REQUIRED (see "The Millennium's own
  modem");
- SM-2: no tone (`--answer-tone-ms 0`, straight to the 2400 Hz carrier),
  with that table.

Only if these fail, it tries a real Bell 2225 Hz answer (SM-3), in this
order: a datamodem release (or our patched datamodem build, DS-3) with
U2; otherwise el-modem's
`AT+MS=B212` profile, on an x86-64 host, with `EL_MODEM_RECORDING=0`
enforced and the SPEC §7 controls applied to it (DS-4). If none of them
connects reliably, the soft modem path is dropped and the hardware
modem remains the supported path. No CARDMAN decision depends on the
soft modem.

**S5. The soft-modem host is a card-data host.** Every control in
`SPEC.md` §7 is required, mainly these:

- datamodem log level at most `info`, and `DATAMODEM_RECORD` never set
  (a call recording is a decodable raw sink, rule 4);
- shim socket mode 0600 in a 0700 directory; the shim never logs
  payload bytes;
- SIP and RTP bound to an isolated voice LAN or VLAN (`--bind-addr`);
- the Asterisk route to the modem endpoint reachable only from the
  payphone ATA's endpoint. Anything that can reach the modem can act as
  a terminal, which is the DLOG-emulator threat of ADR-0001 review A-1;
  the bridge allow-list and attempt limits (ADR-0001 W1) still apply
  behind it.

**S6. Send changes upstream, and keep none locally.**

- U1: auto-answer (`ATS0=1`, `RING`) in `datamodem modem --port`. It
  would let mm_manager drive datamodem over a PTY pair directly and
  retire the shim.
- U2: a Bell 212A answer option using spandsp's
  `MODEM_CONNECT_TONES_BELL_ANS`. cryan209/v90modem's LGPL-2.1 changes
  to spandsp `v22bis_tx.c`/`v22bis_rx.c` are a working reference for
  it (#15). The same change could be offered to freeswitch/spandsp,
  where the flag has been declared but unused. **U2 is on the critical
  path** (design review H1). Without it, every soft-modem terminal
  depends on a per-terminal MODEM table and a 2225 Hz recovery path
  (S7). So this project prepares the patch and offers it upstream,
  rather than only filing a request.
- U3: a documented guarantee that no log level writes line data.

All three go to the datamodem maintainer (and the spandsp half of U2 to
freeswitch/spandsp) under their own licences. This repo does not carry
patches to datamodem. A patch prepared for U2 lives in a fork of
datamodem, not here.

**S7. A 2225 Hz recovery path stays reachable, and each modem gets its
own mm_manager.** This resolves design review H1 and H2.

- Until a datamodem release with U2 is deployed, every soft-modem site
  keeps one 2225 Hz answerer (the hardware modem, or el-modem per DS-4)
  reachable from the dialplan. A terminal that reverts to its default
  MODEM table, after memory loss or a re-install, needs a Bell answer
  tone to call in at all. Without that answerer it could never download
  the fix.
- Asterisk chooses the modem per terminal, from a variable set on the
  ATA endpoint. That is the bootstrap route, the migration switch and
  the rollback (`SPEC.md` §3.2, §3.6).
- Each modem has its own mm_manager instance, with its own working
  directory and therefore its own `mm_manager.db` (mm_manager opens it in
  the current directory, `src/mm_manager.c` L587). Both instances share
  the table directories and the loopback mm-bridge. Because a terminal is
  routed to one modem at a time, the two instances never serve the same
  terminal at once.

## Owner decisions (open)

| ID | Question | Recommendation |
|---|---|---|
| DS-1 | Accept a GPL-3.0-or-later component on the payphone host, at a process boundary only (S3)? | Yes. Nothing from it is linked into or shipped with this fork. |
| DS-2 | Shim language: Python (like `tools/cardman_bridge.py`) or C (like `src/`)? | Python: stdlib `pty`, `socket` and `selectors` only, no third-party packages, same review style as the bridge. |
| DS-3 | How to pursue U2 (Bell answer tone)? | Prepare the patch ourselves from the v90modem LGPL reference, test it with `datamodem selftest` against a Bell 212A originator, and offer it to datamodem now. It is on the critical path (S6). |
| DS-5 | For soft-modem terminals, send a per-terminal MODEM table (`tables/<terminal_id>/mm_table_21.bin`) with ANSWER TONE NOT REQUIRED, bootstrapped by one call-in through a 2225 Hz answerer (hardware modem or el-modem)? | Yes, **as an interim measure until U2 ships**. Per terminal and never in `tables/default`, so hardware-modem terminals keep the Nortel default. Generate the file with the table tool (`SPEC.md` T-7), not by hand-editing bytes. |
| DS-4 | If SM-1 and SM-2 fail and no datamodem U2 release exists, try el-modem for SM-3 on an x86-64 host? | Yes, for the test only. Adopt it in production only if it passes SM-1 to SM-8, recording is verified off (SM-8 decode check over its capture directory), and its provenance review (#14) is accepted. |

## Consequences

- Day-to-day call-ins need no physical modem. **Until U2 ships,
  though, a site still needs a 2225 Hz answerer** for bootstrap and
  recovery (S7): the hardware modem, or el-modem on an x86-64 host. Once
  a datamodem release with U2 is deployed, the per-terminal MODEM table
  and the recovery answerer can both be retired. The hardware modem path
  is unchanged throughout.
- **Reset risk (interim only):** a terminal that loses its downloaded
  MODEM table reverts to "answer tone required" and stops reaching
  datamodem. Recovery is an operator switch to the 2225 Hz route
  (`SPEC.md` §3.6), and the missed-call-in alarm (`SPEC.md` §4.5) is
  how the operator finds out.
- Two mm_manager instances, one per modem, split the local masked
  history across two `mm_manager.db` files. CARDMAN, via the bridge,
  stays the system of record, so nothing billable depends on the local
  databases.
- One more process holds card data in memory (datamodem), plus the
  shim, which this repo owns. `THREAT_MODEL.md` gains four rows and a
  refinement of trust boundary 1 (`SPEC.md` §7), and each control C-1 to
  C-7 names the threat-model sink or boundary it covers.
- The soft modem fits ADR-0001 without changing its planes (`SPEC.md`
  §3.7). It is a line-side front end under mm_manager, run as its own
  user, with no path to the bridge's socket or secrets. Its health
  events are three new PIV v1 event types, all S1
  (`../wapi-bridge/VOCABULARY.md` §4.7, §5.1, §6.6-6.7), delivered by
  `mm-termcfg`'s relay from a shim spool. Table `0x21` stays
  operator-owned, outside the config plane's managed set.
- `COMPATIBILITY_PLAN.md` §1's `app_softmodem -> TCP -> socat PTY` box
  is replaced by this design, and its §5 risk now points to S4.
- The soft modem's audio path becomes a new failure source: codec,
  jitter buffer, ATA echo canceller. `SPEC.md` §3 makes each of them an
  explicit configuration item, and datamodem's own debug log and
  `selftest` give a way to isolate the modem from the path.
- If datamodem stalls upstream, the fork is no worse off: the shim's
  contract is plain bytes over a Unix socket, and any modem that can run
  a per-call command (or, after U1, act as an auto-answer Hayes modem)
  can replace it.

## Design review (2026-10-26)

The author reviewed this design against mm_manager's code and the
terminal-modem findings above. Every finding is dispositioned:

| ID | Sev | Finding | Disposition |
|---|---|---|---|
| H1 | High | The fix (DS-5) needs a 2225 Hz bootstrap per terminal, and a terminal that resets to its default MODEM table cannot reach datamodem or download the fix. "No physical modem is needed" was overstated. | **Fixed**: U2 is critical path (S6, DS-3); a 2225 Hz recovery path stays reachable (S7); Consequences corrected, with the reset risk stated. |
| H2 | High | A mixed fleet (bootstrap on hardware, steady state on datamodem) needs two mm_manager instances, which would share `mm_manager.db` in the working directory (`src/mm_manager.c` L587). The dialplan had a single route. | **Fixed**: S7; `SPEC.md` §3.2 routes per ATA endpoint, and §3.6 covers instances, migration and rollback. |
| M1 | Med | With `-w`, mm_manager notices a dead session only through about 30 s of timeouts. A call-in during that window has its `RING`/`CONNECT` swallowed inside the protocol loop. | **Fixed**: shim `COOLDOWN` state, refusing calls for 35 s after any call ends (`SPEC.md` §4.1, §4.2). A `+++`/`ATH` hang-up (building without `USE_MODEM_DTR`) is recorded as the fallback if the cooldown proves too slow. |
| M2 | Med | No test ran the real mm_manager binary against the shim (its termios on a PTY, `CRTSCTS`, `RING`/`CONNECT` parsing, the `-w` path). | **Fixed**: T-6. |
| M3 | Med | Registration loss or repeated refusals would show only as terminal Error 24 in the field. | **Fixed**: `SPEC.md` §4.5 health signals and alarms (counts only, via the ADR-0001 relay). |
| M4 | Med | SM-1b could not tell a wrong table bit from a V.22 handshake failure. | **Fixed**: SM-1a proves the bit with a hardware modem whose answer tone is suppressed. |
| L1 | Low | One call per datamodem instance was not stated as a capacity limit. | **Fixed**: `SPEC.md` §3.3 note. |
| L2 | Low | The MODEM table had to be generated by a tool, but no test covered it. | **Fixed**: T-7 round-trip test; tool in SM-P1. |

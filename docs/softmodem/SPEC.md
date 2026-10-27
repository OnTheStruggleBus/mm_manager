# Soft modem integration spec (datamodem)

Implements [ADR-0002](ADR-0002-SOFT-MODEM.md). Docs only; nothing here is
built yet. Upstream facts are cited as `repo@commit path Lnn`. They were
checked on 2026-10-26 against datamodem `1aa248a` (release v1.1.0), so
re-check them when the pinned release changes.

## 1. Scope

In scope: the leg from the terminal's phone line to mm_manager's modem
device when that leg runs over VoIP; the `mm-softmodem-dte` shim; the
configuration of the ATA, Asterisk and datamodem; security controls;
tests.

Out of scope: everything after mm_manager (bridge, CARDMAN, ADR-0001).
Also out of scope: any change to `src/`, and changes to datamodem itself
(proposed upstream as U1-U3 in §11).

## 2. Topology

```
Nortel Millennium terminal
   |  analogue line (Tip/Ring), Bell 212A originate, 1200 bit/s
   v
[ ATA, FXS port ]          G.711 u-law only; no VAD/CNG/PLC/echo canceller
   |  SIP + RTP on the voice LAN/VLAN (isolated, §7)
   v
[ Asterisk ]               existing PBX; routes the manager number to one endpoint
   |  SIP + RTP, G.711 u-law, no transcoding, no jitter buffer
   v
[ datamodem answer ]       GPL-3.0+, separate process, V.22 1200 answer, 8N1, V.42 off
   |  per call: --exec "mm-softmodem-dte connect"  (the line on stdin/stdout)
   v
/run/mm-softmodem/sock/dte.sock     Unix socket, 0600
   v
[ mm-softmodem-dte serve ] MIT, tools/ in this repo; persistent
   |  PTY, presents a minimal Hayes modem
   v
/run/mm-softmodem/ttyMM -> /dev/pts/N
   v
mm_manager -m -w -f /run/mm-softmodem/ttyMM ...   (unchanged binary)
   |  loopback, unchanged
   v
mm-bridge -> CARDMAN
```

Until U2 ships, Asterisk can instead route a terminal to a 2225 Hz
answerer (`mm-bell`: hardware modem or el-modem) feeding a second
mm_manager instance, for bootstrap and recovery (§3.2, §3.6).

Process and licence boundaries are the arrows: nothing in the chain is
linked to anything else in it (ADR-0002 S3).

## 3. Components and configuration

### 3.1 ATA

Any FXS ATA that can be held to the following. Each item is a known way
a modem call fails over VoIP (datamodem README "What the call needs"):

| Setting | Value | Why |
|---|---|---|
| Codec | G.711 u-law only (PCMU) | A compressed codec destroys the modem signal; North American default. |
| VAD / silence suppression / CNG | Off | Removes carrier during quiet moments. |
| Packet loss concealment | Off | Invents audio. |
| Line echo canceller | Off for this port (or "modem/fax passthrough" if offered) | Damages DPSK; V.22 does not need it. |
| Jitter buffer | Fixed, smallest stable (start at 60 ms) | An adaptive buffer stretches or drops frames. |
| T.38 / fax re-INVITE | Off | Would replace the audio path mid-call. |
| Dial plan | Pass the terminal's dialled manager number through unchanged | Asterisk routes on it. |
| Registration | To Asterisk, as its own endpoint (`payphone-ata-<n>`) | Lets §3.2 restrict who can reach the modem. |
| Answer supervision | **Polarity reversal on answer: on** | The terminal needs it. Without it, a Conexant modem behind an HT802 saw "Carrier lost" and the terminal showed Error 51 ([hharte/mm_manager#25](https://github.com/hharte/mm_manager/issues/25); upstream README "Phone Line"). |

### 3.1a Terminal MODEM table (DLOG `0x21`)

The terminal's own modem is a TDK/Silicon Systems 73K212 (Bell
212A/103 only) or 73K324L, depending on the control board. Its firmware
needs an answer tone unless told otherwise (ADR-0002 "The Millennium's
own modem"). For a soft-modem terminal, mm_manager sends a per-terminal
`tables/<terminal_id>/mm_table_21.bin` (DS-5), identical to
`tables/default/mm_table_21.bin` except:

| Offset | Field (Nortel MSR 2.1 p. 2-288/289) | Default | Soft-modem value |
|---|---|---|---|
| 33 | Answer-tone config byte: bit 1 ENABLE ANSWER TONE DETECT, bit 2 ANSWER TONE NOT REQUIRED (bit order inferred) | `0x02` | `0x06` (detect if present, proceed on carrier) |

Bootstrap: the terminal uses this only after a successful call-in has
downloaded it. Do the first install through a 2225 Hz answerer (the
hardware modem, or el-modem per DS-4), then move the terminal to
datamodem. A small generator script, not hand-edited bytes, should
produce the file. It belongs with the SM-P1 task card.

### 3.2 Asterisk

Two PJSIP endpoints and one route. Illustrative values; keep secrets
out of this repo (rule 8).

```ini
; pjsip.conf
[payphone-ata-1]
type = endpoint
context = from-payphone
allow = !all,ulaw
dtmf_mode = rfc4733        ; no in-band DTMF detector on the audio
fax_detect = no
direct_media = no

[mm-modem]
type = endpoint
context = no-outbound      ; the modem never places calls
allow = !all,ulaw
dtmf_mode = rfc4733
fax_detect = no
direct_media = no
```

Each payphone ATA endpoint also carries which modem its terminal uses
(ADR-0002 S7). This is the bootstrap route, the migration switch and the
rollback:

```ini
; pjsip.conf, on each [payphone-ata-<n>] endpoint
set_var = MM_MODEM=soft          ; or MM_MODEM=bell: hardware modem / el-modem (2225 Hz)

[mm-bell]                        ; the 2225 Hz answerer, e.g. a hardware modem on an ATA FXS port
type = endpoint
context = no-outbound
allow = !all,ulaw
```

```ini
; extensions.conf
[from-payphone]
; <manager number> is what the terminal was installed with (README, "Key
; in the Manager's phone number"). Only this context reaches the modems.
exten => <manager number>,1,GotoIf($["${MM_MODEM}" = "bell"]?bell)
 same => n,Dial(PJSIP/mm-modem,30)
 same => n,Hangup()
 same => n(bell),Dial(PJSIP/mm-bell,30)
 same => n,Hangup()

[no-outbound]
exten => _X.,1,Hangup()
```

Rules:

- An unset `MM_MODEM` routes to datamodem. Set `bell` for a terminal's
  bootstrap and for recovery (§3.6).
- `mm-modem` and `mm-bell` are reachable from `from-payphone` only. No other context,
  trunk, or extension may route to it (§7, C-6).
- No `JITTERBUFFER()`, no `MixMonitor`/`Monitor`, and no call recording
  on this route: a recording of the audio is a decodable card-data sink
  (rule 4).
- No transcoding: both legs are `ulaw`. Asterisk passes the audio
  through, and a `core show channel` during a call must show `ulaw` on
  both sides.

### 3.3 datamodem

Install a pinned upstream release (v1.1.0 at time of writing) and record
its SHA-256 in the deployment notes (§8). Run it under its own Unix
user, `mm-modem`. Config file (`/etc/mm-softmodem/datamodem.conf`, mode
0640, owned by root, group `mm-modem`). The password comes from the
environment (`DATAMODEM_PASSWORD`, set in a root-only systemd
credential), never from this file:

```ini
# Registration to the local Asterisk
server         = 127.0.0.1
username       = mm-modem
bind-addr      = 127.0.0.1          # or the voice-VLAN address if Asterisk is elsewhere
register       = on

# Media
codec          = pcmu
jitter-buffer-ms = 60
rtp-port       = 40000
rtp-port-range = 20

# The Millennium: Bell 212A family, 1200 bit/s, plain async 8N1
modulation     = v22
step-down      = off
v42            = off
data-bits      = 8
parity         = none
stop-bits      = 1
answer-tone-ms = 3300               # hardware matrix §9.2: 3300 (SM-1) or 0 (SM-2)

# Calls
calls          = 0                  # keep answering
train-timeout  = 45
idle-timeout   = 90                 # backstop; the shim hangs up first (§4.3)
max-call       = 1800               # backstop on a stuck call

# Diagnostics: never above info (§7, C-2)
log-level      = info
tui            = off

exec           = /opt/mm/tools/mm_softmodem_dte.py connect --socket /run/mm-softmodem/sock/dte.sock
```

Every key above is a datamodem option name (`src/config.c`, the option
table). Flags behave as documented in the datamodem README, sections
"Using it", "Answering for a BBS", "Modulations" and "What the call
needs". The environment must never contain `DATAMODEM_RECORD`
(`src/modem.c` L2482 reads it to record call audio).

Capacity: datamodem takes one call at a time and answers a second with
`486 Busy Here` (README "Answering for a BBS"). A terminal that gets
busy retries according to its call-in parameters. Run one datamodem,
shim and mm_manager set per simultaneous call-in you expect. Each set
needs its own SIP account, `local-port`, RTP range, socket, PTY and
working directory.

### 3.4 `mm-softmodem-dte` (new, this repo)

`tools/mm_softmodem_dte.py`, MIT, Python 3 stdlib only (`pty`, `os`,
`socket`, `selectors`, `termios`, `tty`, `logging`) per DS-2. Two
subcommands:

- `serve`: the persistent daemon (systemd unit `mm-softmodem-dte`).
  It owns the PTY and the socket. One instance per modem line.
- `connect`: what datamodem runs per call. It connects to the socket,
  sends the one-line header (§4.2), then copies stdin to the socket and
  the socket to stdout until either side ends. It does no logging of
  data and no buffering beyond one `read()`.

Behaviour is specified in §4. Size target: under 300 lines including
the argument parser, so the whole card-data path in it can be reviewed
at once.

### 3.5 mm_manager

Unchanged binary, any CARDMAN build:

```sh
mm_manager -m -w -f /run/mm-softmodem/ttyMM -n <ncc number> [usual options]
```

- `-m`: modem mode, so `init_modem()` and `wait_for_modem_response()`
  run (`src/mm_connection.c` `mm_connection_open()`).
- `-w`: do not check DCD/RI. A PTY has no modem-status lines, so with
  carrier monitoring on, `receive_mm_packet()` would report "Carrier
  lost" on the first byte (`src/mm_proto.c`, `monitor_carrier`).
- `-i` is not needed: the shim answers `OK` to any well-formed `AT`
  line. The default `+MS=B212` is accepted and ignored. Modulation is
  set by datamodem (§3.3), and the shim logs that once at start.
- `-b` is irrelevant on a PTY (the default 19200 passes the
  `baudrate < 1200` check).

### 3.6 Instances, migration and recovery

One mm_manager per modem (ADR-0002 S7). Each runs in its own working
directory, so each gets its own `mm_manager.db` (`src/mm_manager.c`
L587). Both share the table directories and the bridge:

| Instance | Working dir | Modem device | Flags |
|---|---|---|---|
| `mm-manager-soft` | `/var/lib/mm-manager/soft` | `/run/mm-softmodem/ttyMM` (shim) | `-m -w -d /opt/mm/tables/default -t /opt/mm/tables` |
| `mm-manager-bell` | `/var/lib/mm-manager/bell` | hardware modem tty (or el-modem's PTY, with its `-i`) | `-m -d /opt/mm/tables/default -t /opt/mm/tables` (plus `-w` on a PTY) |

A terminal is routed to one modem at a time (§3.2), so the two
instances never write the same `tables/<terminal_id>/` at once.

**Moving a terminal to the soft modem (interim, until U2):**

1. Set `MM_MODEM=bell` on the terminal's ATA endpoint.
2. Generate `tables/<terminal_id>/mm_table_21.bin` with the table tool
   (§3.1a, T-7).
3. Have the terminal call in for a table update, and confirm in
   `tables/<terminal_id>/table_update.log` that table `0x21` was ACKed.
4. Remove `MM_MODEM` (the soft route) and do one test call-in.

**Rollback or recovery:** set `MM_MODEM=bell` again. Use it when a
terminal misses its expected call-in (§4.5), for example after it has
lost its downloaded MODEM table. Once a datamodem release with U2 is
deployed, steps 1-3 and the `bell` route are no longer needed for new
terminals. Keep `mm-bell` until every terminal has proven it can call in
on U2.

## 4. Interface contracts

### 4.1 Shim to mm_manager (the PTY)

The shim opens a PTY pair, puts the slave in raw mode (`tty.setraw`), and
points `/run/mm-softmodem/ttyMM` at the slave. It keeps the master open
for its whole life. mm_manager then applies its own termios
(`init_serial()`), which is harmless on a PTY.

States:

| State | Input from mm_manager | Output to mm_manager | Leaves on |
|---|---|---|---|
| `IDLE` | Line-buffered, CR-terminated. A line that matches `^[Aa][Tt][\x20-\x7e]{0,78}$` gets `\r\nOK\r\n`. Anything else gets `\r\nERROR\r\n`. A line over 80 bytes is discarded up to the next CR and answered `ERROR`. No echo. | Only the replies above | A call arrives (§4.2): go to `RINGING` |
| `RINGING` | Discarded | `\r\nRING\r\n`, then after 100 ms `\r\nCONNECT 1200\r\n` | Immediately to `ONLINE` |
| `ONLINE` | Every byte, unmodified, to the call socket | Every byte from the call socket, unmodified | Call socket EOF or error; idle timeout (§4.3); shim shutdown. Then close the call socket, wait 1 s of silence, send `\r\nNO CARRIER\r\n`, go to `COOLDOWN` |
| `COOLDOWN` | As `IDLE` (AT lines answered) | As `IDLE` | After `--cooldown` (default 35 s): go to `IDLE`. Calls arriving meanwhile are refused (§4.2). |

Why `COOLDOWN`: with `-w`, mm_manager finds out a session died only
through protocol timeouts: 3 retries × `PKT_TIMEOUT_MAX` (10) × a 1 s
read (`VTIME = 10`), about 30 s. During that time it is still in the
packet loop, not reading result codes. A `RING`/`CONNECT` sent then
would be thrown away as noise and the call-in lost (design review M1).
35 s covers that window. A refused caller gets busy and retries. If the
cooldown proves too slow in SM-6, the fallback is to build without
`USE_MODEM_DTR` (`src/mm_modem.c` `hangup_modem()`), so mm_manager hangs
up with `+++`/`ATH` and the shim can treat that as an exact "back in
command mode" signal. That is a one-line upstream-file change, held in
reserve.

Why this is enough: mm_manager matches result codes by substring
(`wait_for_modem_response()`, `strstr` against `modem_responses[]`).
It treats `RING` as "keep waiting" and `CONNECT` as "start the
protocol" (`mm_connection_wait()`). Its hang-up is a DTR drop
(`hangup_modem()`, `USE_MODEM_DTR`), which is a silent no-op on a PTY.
The shim therefore cannot see a hang-up from mm_manager, and it ends
calls itself (§4.3).

`CONNECT` reports the fixed rate `1200`. The shim does not trust the
header for anything mm_manager acts on; it only checks it (§4.2).

### 4.2 datamodem to shim (the per-call socket)

- Socket: `AF_UNIX`, `SOCK_STREAM`, `/run/mm-softmodem/sock/dte.sock`,
  mode 0600, owner `mm-modem`. Its directory is 0700, same owner (kept
  apart from the PTY symlink, which mm_manager's user must reach). The shim
  runs as `mm-modem`, and datamodem's `connect` runs as `mm-modem`. The
  shim checks `SO_PEERCRED` and accepts only its own uid.
- First line from `connect` (ASCII, LF-terminated, at most 128 bytes):
  `MMDTE/1 rate=<MODEM_RATE> mod=<MODEM_MODULATION>`. The values come
  from the environment datamodem sets for `--exec` (README, "Answering
  for a BBS": `MODEM_RATE`, `MODEM_MODULATION`). `CALLER_ID` is not
  sent. It is the ATA line's number, logged by Asterisk already, and
  it has no use here.
- The shim **refuses** the call (closes the socket at once, which makes
  `connect` exit, so datamodem clears the call) when any of these hold:
  - the header is malformed, or `mod` is not `v22`, or `rate` is not
    `1200`;
  - the shim is already `ONLINE` (datamodem also answers 486 to a
    second call; this is defence in depth);
  - the shim is in `COOLDOWN` (§4.1);
  - no process has the PTY slave open (the master reports `POLLHUP`):
    there is no mm_manager to take the call.
- After the header the stream is raw bytes both ways.

### 4.3 Ending a call

| Who ends it | What happens |
|---|---|
| Terminal hangs up (normal) | datamodem waits 4 s for carrier, then ends `connect`'s stdin and sends it SIGHUP (README "Answering for a BBS"). `connect` closes the socket, and the shim goes `ONLINE` -> `NO CARRIER` -> `IDLE`. |
| mm_manager gives up (protocol timeout or error) | mm_manager stops sending. The shim's **idle timer** (`--idle-timeout`, default 40 s) fires when nothing has come from mm_manager for that long while `ONLINE`. The shim closes the call socket, `connect` exits, and datamodem clears the call (an `--exec` command exiting ends the call). 40 s exceeds mm_manager's worst silent stretch inside a live session: 3 retries x `PKT_TIMEOUT_MAX` (10) x a 1 s read (`VTIME = 10`, `src/mm_serial_posix.c`). |
| Line dies without a clean hang-up | datamodem's `idle-timeout` (90 s) and `max-call` (1800 s) clear it. |
| mm_manager exits | The PTY master sees `POLLHUP` and the shim ends any call at once. New calls are refused until a reader reopens the slave. |
| Shim exits | systemd restarts it. Calls in progress end (socket EOF), and mm_manager sees `NO CARRIER` or a protocol timeout. |

### 4.4 Shim logging

Allowed, at INFO: start-up (PTY path, socket path, "init strings
ignored"); per call: accepted or refused, with the reason; header
`mod`/`rate`; duration; byte **counts** each way; how it ended. Never
allowed, at any level: payload bytes in any form (hex, ASCII, escaped,
or lengths of individual reads that could reconstruct framing), AT
lines (they are harmless today, but logging them invites logging
everything on the PTY), or anything from the call socket after the
header. There is no debug flag that changes this (rule 4).

### 4.5 Health signals and alarms

Without these, every failure on this path shows up only as a terminal
Error 24 in the field (design review M3). Every signal is a count or a
state, never line data (S0 in ADR-0001's vocabulary):

| Signal | Source | Alarm when |
|---|---|---|
| `mm-modem` reachable | Asterisk AOR `qualify_frequency` on the datamodem endpoint | Unreachable for more than 2 minutes (lost registration or crash) |
| Calls accepted and refused, by reason (`header`, `busy`, `cooldown`, `no_reader`, `peer`) | Shim INFO log, also as counters | Any `no_reader` or `peer`; `cooldown` refusals more than 3 per hour (M1 window too wide) |
| Calls that trained but ended with no mm_manager traffic | Shim (call ended by idle timer with 0 bytes from mm_manager) | Any |
| datamodem training failures | datamodem INFO log (`NOCARRIER`-type outcomes) | Several in a row from one terminal's ATA |
| Missed expected call-in | Last call-in time per terminal (mm_manager log or `mm_manager.db`) against its call-in schedule | Past the schedule plus one retry interval: switch that terminal to `MM_MODEM=bell` (§3.6) |

Events go out through ADR-0001's event relay (`mm-termcfg`). The shim
itself makes no network calls: a log shipper or the relay reads its
counters.

## 5. Call flow

```
terminal      ATA/Asterisk        datamodem              shim                   mm_manager
   | dial mgr no. ->| INVITE ------------>|                    |                        |
   |                |<------ 200 OK ------|                    |                        |
   |<== 2100 Hz answer tone (3.3 s, or none: §9.2) ==|         |                        |
   |<== V.22 handshake / training ==================>|         |                        |
   |                |                     | exec connect ----->| check header, peer,    |
   |                |                     |                    | slave open             |
   |                |                     |                    |--- RING -------------->|
   |                |                     |                    |--- CONNECT 1200 ------>| proto_connect()
   |== DLOG frames (incl. PAN/PIN in auth) ==========>|=======>|=======================>| -> bridge
   |<========================= DLOG frames ===========|<=======|<=======================|
   | on-hook ------>| BYE --------------->| carrier lost 4 s;  |                        |
   |                |                     | stdin EOF, SIGHUP  |                        |
   |                |                     |       connect exits|-- 1 s, NO CARRIER ---->| (timeout/disconnect)
```

Timing to verify on hardware (§9.2): the terminal's patience from
off-hook-answered to carrier detect. With datamodem the answer tone
takes 3.3 s, and V.22bis-family training "about 6" s (datamodem README
"Modulations").

## 6. Failure modes and where to look

| Symptom | Likely cause | Check |
|---|---|---|
| datamodem logs carrier, then mm_manager sees a run of one byte value | Demodulator never locked; datamodem warns about this itself (`src/session.c` L347-356) | Transcoding or a lossy codec somewhere: `core show channel` both legs `ulaw`; ATA settings §3.1 |
| Never trains | Audio path, or the terminal rejects a V.22 answerer | Run SM-1/SM-2; keep one datamodem `--log-level debug` run **with test cards only and logs destroyed after** (§7, C-2) |
| `RING`/`CONNECT` never reach mm_manager | Shim refused: header, busy, or no PTY reader | Shim INFO log reason |
| Session stalls mid-download | RTP loss or jitter beyond the fixed buffer | Raise `jitter-buffer-ms` stepwise; check the voice VLAN |
| Call stays up after mm_manager errors | Idle timer too long, or mm_manager still sending | Shim log; mm_manager retries |

## 7. Security controls

Card data on this leg is pre-existing: the terminal sends full PAN and
PIN over its line whatever modem answers (`THREAT_MODEL.md`, "Wire
to/from terminal"). What is new is that the line is now packets on a
network and processes on a host.

| ID | Control | Rule | Verified by |
|---|---|---|---|
| C-1 | Voice LAN/VLAN isolated: only the ATA(s), Asterisk, and the payphone host. No route from general LAN or Internet to the RTP range or SIP port. datamodem and Asterisk bind there or to loopback (`bind-addr`). RTP is treated as clear text: SRTP was not verified for datamodem. | 5 (spirit), threat model | Network diagram in deployment notes; `ss -lunp` shows no wildcard binds |
| C-2 | datamodem `log-level` at most `info`; `DATAMODEM_RECORD` never set (systemd `UnsetEnvironment=DATAMODEM_RECORD`). A debug run is allowed only with test cards, and its log is deleted after use. | 4 | Unit-file review; T-5 decode check |
| C-3 | No Asterisk recording, monitoring, or `core set debug` capture on the `mm-modem` route in production. | 4 | Dialplan review |
| C-4 | Shim never logs payload (§4.4); socket 0600 in a 0700 dir; `SO_PEERCRED` uid check; PTY slave and its symlink directory group `mm-dte` (the shim's and mm_manager's users only), mode 0660. | 2, 3, 4 | T-2 tests; `ls -l` checks in T-4 |
| C-5 | Processes run as dedicated users: `mm-modem` (datamodem and shim), the existing mm_manager user. Neither can read the bridge's credentials. | 8 | Unit files |
| C-6 | Only `from-payphone` can reach `mm-modem` and `mm-bell`. Anything that reaches the modem can act as a terminal (ADR-0001 review A-1); the bridge allow-list and attempt limits (ADR-0001 W1) remain the card-side control. | ADR-0001 W1 | Dialplan review; attempt from another endpoint is rejected (T-4) |
| C-7 | datamodem and shim process memory holds clear PAN/PIN in transit, the same class as mm_manager's (`THREAT_MODEL.md` "Process memory"). No core dumps for either (`LimitCORE=0`). | 2, 3 | Unit files |

Rows to add to `THREAT_MODEL.md` when this is built:

| Sink | Contents | Required control |
|---|---|---|
| RTP audio on the voice network | Modulated PAN/PIN (decodable) | C-1 |
| datamodem logs and call recordings | Line data at `debug`/`trace`; audio if `DATAMODEM_RECORD` set | C-2, C-3 |
| Shim socket and PTY | Clear PAN/PIN in transit | C-4, C-5 |

## 8. Licence and supply chain

- datamodem: GPL-3.0-or-later; its release binaries link pjproject
  (GPLv2+), spandsp (LGPL-2.1) and OpenSSL (Apache-2.0), and carry those
  licences under `licenses/`, with the dependency sources attached to
  each release (datamodem README "Licence"). Fully open, no binary-only
  parts.
- This repo neither vendors nor redistributes it, and neither CI nor
  `release.yml` fetches it (ADR-0002 S3). Operators install a release
  themselves.
- Pin: the deployment notes record the release tag, the asset name, and
  the SHA-256 of the archive as downloaded. Upgrades go through T-1 and
  SM-4 again before production.
- The shim is MIT, like the rest of this fork. It has no third-party
  dependencies.
- Asterisk (GPLv2) and datamodem (GPLv3) talk only SIP/RTP over a
  socket. They are separate programs, so no licence combination arises.

## 9. Tests

### 9.1 Without hardware

| # | Test | Pass when |
|---|---|---|
| T-1 | `datamodem selftest --modulation v22 --no-step-down` on the target host (command form from datamodem DEVNOTES "Testing") | Exits 0 |
| T-2 | Shim unit tests (`tools/test_mm_softmodem_dte.py`, stdlib `unittest`), using a real PTY pair and socket: `AT`/`ATZ`/default init string answer `OK`; non-AT and over-long lines answer `ERROR`; a call gives `RING` then `CONNECT 1200`; all 256 byte values pass both ways unchanged; socket EOF gives `NO CARRIER` and `IDLE`; idle timeout closes the call; a second call is refused while `ONLINE`; a call during `COOLDOWN` is refused, and one after it is accepted; a call with no PTY reader is refused; a bad header, `mod` or `rate` is refused; a wrong peer uid is refused; refusal counters increment by reason | All pass; runs in the existing Linux `ctest` step |
| T-6 | **Real mm_manager against the shim** (design review M2): start the built `mm_manager -m -w -f <shim PTY>` and a fake caller on the socket that sends terminal-side bytes from a synthetic fixture (test PAN only). Then end the call abnormally, and place a second call after the cooldown. | mm_manager's own init succeeds through the shim (its termios, including `CRTSCTS`, on a PTY); it logs "Connected!" and handles the first packet; after the abnormal end it returns to "Waiting for call" before the cooldown ends; the second call connects |
| T-7 | MODEM table tool round trip (design review L2): generate `mm_table_21.bin` from named fields, decode it against Nortel MSR 2.1's field list, and compare with `tables/default/mm_table_21.bin` | With default inputs, the output is byte-identical to the default file; with "answer tone not required", only byte 33 differs (`0x02` -> `0x06`) |
| T-3 | Transport through the real chain without a terminal: a second datamodem (`dial <manager number> --modulation v22 --no-step-down --v42 off --exec <script>`) registered as a test endpoint in `from-payphone` sends a fixed 4 KiB pattern; a test reader on the PTY echoes it back | Pattern returns byte-exact; shim logs counts 4096/4096 |
| T-4 | Negative routing and permissions: same test endpoint placed in another context cannot reach `mm-modem`; `stat` of socket, directory, and PTY shows §4.2/C-4 modes | Call rejected; modes as specified |
| T-5 | Leak check after T-3 with a pattern containing test PAN `4012888888881881` and a dummy PIN, decoded per rule 9 (ASCII and packed-BCD forms) over the datamodem log, journald for all three units, and the shim log | No match |

### 9.2 Hardware (to add to `HARDWARE_TEST_PLAN.md` as section 4)

| # | Test | Do | Pass when | If it fails |
|---|---|---|---|---|
| SM-0 | Identify the terminal's modem | Open the terminal; record the control board revision, the modem chip marking (73K212 or 73K324L), and the firmware from `DLOG_MT_SW_VERSION`. | Recorded | — |
| SM-1 | V.22 answer, default tone, default MODEM table | `answer-tone-ms = 3300`, terminal on the default `mm_table_21.bin`. 10 consecutive call-ins (craft "call manager"). Expected to fail on a 73K212, which cannot detect 2100 Hz. | 10/10 reach `CONNECT` and complete the session; datamodem's `carrier established` line shows `modulation=v22 rate=1200` (`src/modem.c` L1966) | Record the result, then go to SM-1a. |
| SM-1a | Prove the MODEM table bit (design review M4) | On the `bell` route, set the hardware modem to answer **without** an answer tone (for example, a longer `S7` with the answer tone disabled per its manual; record the exact command). Call in with the default table, then download the T-7 "not required" table and call in again. | Default table: no connect. "Not required" table: connects. This proves the byte-33 bit independently of V.22. | If both connect, the terminal does not enforce the tone, so SM-1's result came from something else. If neither connects, the bit order is wrong: fix T-7 before SM-1b. |
| SM-1b | V.22 answer, MODEM table "answer tone not required" | Install once through a 2225 Hz answerer (hardware modem) with the §3.1a per-terminal table, confirm the terminal ACKs table `0x21`, then move it to datamodem. `answer-tone-ms = 3300`. 10 call-ins. | 10/10 as SM-1 | Go to SM-2. Keep one datamodem audio recording **of a call-in with no card traffic**, made with the terminal's craft call-in and deleted after analysis, as evidence for U2. |
| SM-2 | V.22 answer, no tone | With the SM-1b table, `answer-tone-ms = 0`, so the 2400 Hz answer carrier starts at once. 10 call-ins. | 10/10 as SM-1 | Go to SM-3. |
| SM-3 | Bell 2225 Hz answer | A datamodem release with U2 if one exists. Otherwise el-modem (ADR-0002 #14, DS-4) on an x86-64 host: `EL_MODEM_RECORDING=0`, PTY backend, `AT+MS=B212,0`, no shim. Run mm_manager with `-m -w -i "ATS0=1S7=3&D2+MS=B212,0"`, since el-modem rejects `ATE=1`. 10 call-ins. | 10/10 as SM-1, and el-modem's capture directory is empty afterwards | Soft modem not supported on this terminal (ADR-0002 S4); record the result. |
| SM-4 | Full install session | With the passing SM-1/2/3 setting: a terminal install (all tables down). | Completes; duration within 25% of the hardware-modem baseline for the same table set | Jitter or loss on the path (§6). |
| SM-5 | Card call end to end | `HARDWARE_TEST_PLAN.md` 3.1 over the soft modem (needs test cards: the blocker in `docs/wapi-bridge/SPEC.md` §14.1). | As 3.1 | Compare with the hardware modem on the same day to separate modem from card path. |
| SM-6 | Terminal hangs up mid-session | Pull the line during a table download. | Shim logs EOF; `NO CARRIER`; next call-in works | Shim state bug. |
| SM-7 | mm_manager killed mid-call | `kill` mm_manager during a session. | Shim ends the call at once (`POLLHUP`); terminal sees the line drop; after restart the next call-in works | Shim `POLLHUP` handling. |
| SM-8 | Leak audit | After SM-5, run T-5's decode check over every log on the host. | No match | Blocker (rule 4). |
| SM-9 | Migration and rollback (§3.6) | Move one terminal bell -> soft by the §3.6 steps, then back to bell, then to soft again. | Every step's call-in completes; `table_update.log` shows `0x21` ACKed; each `mm_manager.db` holds only the sessions routed to it | Routing or instance-separation bug. |
| SM-10 | Power cycle keeps the MODEM table | Power-cycle a soft-modem terminal (with its normal battery backup in place), then let it call in. | Still calls in on the soft route | It lost the table: rely on the recovery route and the §4.5 alarm, and record it as an operating constraint. |

## 10. Phasing

| Phase | Work | Gate |
|---|---|---|
| SM-P0 | Owner decisions DS-1 to DS-5 | Recorded in ADR-0002 |
| SM-P1 | Shim, MODEM table tool, health counters (new task cards in `AI/TASKS.md`); T-2, T-6, T-7 | All green in CI; security checklist in `SECURITY_RULES.md` passes review |
| SM-PU | **Critical path, in parallel with SM-P1:** prepare U2 in a datamodem fork from the v90modem LGPL reference; `datamodem selftest` against a Bell 212A originator; offer it upstream (DS-3) | Patch offered; a build is available for SM-3 |
| SM-P2 | Lab: ATA + Asterisk (both routes) + datamodem + shim + both mm_manager instances; T-1, T-3 to T-5; §4.5 alarms fire on a forced registration loss | All pass |
| SM-P3 | Hardware SM-0 to SM-10, alongside the existing hardware session | SM-1a decides the bit; SM-1, SM-1b or SM-2 (or SM-3) 10/10; SM-4 to SM-10 pass |
| SM-P4 | Docs: `COMPATIBILITY_PLAN.md` §1/§5 final, `THREAT_MODEL.md` rows, README section "Using a soft modem" | Merged |

If SM-P3 fails at the answer-tone matrix, stop at SM-P3. The hardware
modem stays the supported path and nothing else in the roadmap changes.

## 11. Upstream requests (datamodem)

| ID | Request | Benefit here |
|---|---|---|
| U1 | Auto-answer in `datamodem modem --port`: `ATS0=n`, `RING`, `ATA`. The README says today "There is no `ATA`: answering calls is `datamodem answer`." | mm_manager could drive datamodem over a PTY pair directly; the shim retires |
| U2 | A Bell 212A answer option for V.22: answer with spandsp's `MODEM_CONNECT_TONES_BELL_ANS` (2225 Hz) instead of V.25 ANS. Today only Bell 103 gets it (`src/modem.c` L604-606). A working LGPL-2.1 reference is cryan209/v90modem's change to spandsp `v22bis_tx.c`/`v22bis_rx.c` (ADR-0002 #15). | **Critical path** (ADR-0002 S6). It retires the per-terminal MODEM table, the bootstrap step and the recovery route (S7). |
| U3 | State in the docs which log levels can contain line data, and keep `info` free of it | Lets C-2 cite a guarantee instead of a review |

Raise U2 with the SM-1 recording attached (DS-3), and include no card
data in it.

## 12. Acceptance

1. A terminal call-in over VoIP completes an install session and a card
   call end to end through datamodem and the shim (SM-4, SM-5).
2. `src/` is unchanged; the fork's upstream delta does not grow.
3. No log, recording, or capture on the host contains a PAN or PIN in
   any decodable form (T-5, SM-8).
4. Only the payphone ATA's endpoint can reach the modem (T-4).
5. A terminal can be moved to the soft route and back, and the missed-call-in
   alarm fires (SM-9, §4.5).
6. The hardware modem path still works unchanged (regression: one
   call-in with `-m` on a real modem, without `-w`).
7. Nothing from datamodem is vendored, built, or shipped by this repo. (A U2 patch
   lives in a datamodem fork.)

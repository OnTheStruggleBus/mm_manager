# Running the SIP soft modem with a PhreakNet (Asterisk) server

How to answer a Millennium terminal's call-in with mm_manager's built-in
soft modem (`-S`, [ADR-0003](ADR-0003-SIP-SOFTMODEM.md)) on an Asterisk
installed by PhreakScript. Values in `<angle brackets>` are yours to choose.
Keep passwords out of this repo (SECURITY_RULES rule 8).

```
Millennium ─analog─> ATA (FXS) ─SIP/RTP─> Asterisk (PhreakNet) ─SIP/RTP, loopback─> mm_manager -S
```

## 1. ATA port for the payphone

Same as any modem over VoIP (SPEC §3.1):

- G.711 µ-law only. VAD, comfort noise, packet loss concealment and the
  echo canceller off. T.38 / fax re-INVITE off.
- Fixed jitter buffer, about 60 ms.
- **Polarity reversal on answer: on.** Without it the terminal shows
  Error 51.
- Registers to Asterisk as its own endpoint, in a context of its own.

## 2. Asterisk: an endpoint for mm_manager

`pjsip.conf` (or the PhreakScript-managed equivalent):

```ini
[mm-modem]
type = endpoint
context = no-outbound          ; the modem never places calls
allow = !all,ulaw
direct_media = no
dtmf_mode = rfc4733
fax_detect = no
t38_udptl = no
auth = mm-modem
aors = mm-modem

[mm-modem]
type = auth
auth_type = userpass
username = mm-modem
password = <long random secret>

[mm-modem]
type = aor
max_contacts = 1
remove_existing = yes
qualify_frequency = 60         ; host monitoring: alarm if unreachable > 2 min
```

`extensions.conf`, in the payphone ATA's context only:

```ini
[from-payphone]
; <manager number> is the number keyed into the terminal at install.
exten => <manager number>,1,Dial(PJSIP/mm-modem,30)
 same => n,Hangup()

[no-outbound]
exten => _X.,1,Hangup()
```

Rules (SPEC §7, ADR-0003):

- Only `from-payphone` may dial `mm-modem`. Anything that reaches the modem
  can act as a terminal.
- No `MixMonitor`, `Monitor`, call recording, `JITTERBUFFER()` or
  `core set debug` capture on this route: a recording of the audio can be
  decoded back into card numbers and PINs.
- No transcoding: during a call, `core show channels verbose` must show
  `ulaw` on both legs.

Check after `pjsip reload` / `dialplan reload`:

```bash
asterisk -rx "pjsip show endpoint mm-modem"
```

## 3. mm_manager

On the same host as Asterisk (recommended), with the password in the
environment rather than on the command line (where `ps` would show it):

```bash
MM_SIP_PASSWORD='<secret>' CARDMAN_BRIDGE_ADDR=127.0.0.1:8420 \
  ./build/mm_manager -S "mm-modem@127.0.0.1:5060;port=5062;rtp_port=12000" \
  -n <manager number> -d tables/default -t tables
```

- `bind` defaults to `127.0.0.1`, and only the registrar's address
  (`127.0.0.1` here) may send SIP or RTP.
- Pick `port` and `rtp_port` outside Asterisk's own SIP port and RTP range
  (`rtp.conf`, often 10000-20000).
- Do not add `-v`, `-l`, `-p` or `-u` in CARDMAN use: they still write card
  data (rule 4, task T4).
- One call at a time. For a second simultaneous call-in, run another
  instance with its own SIP user, ports and working directory.

If Asterisk is on another host, put both on an isolated voice VLAN and
bind to it explicitly:

```bash
MM_SIP_PASSWORD='<secret>' ./build/mm_manager \
  -S "mm-modem@<asterisk ip>:5060;bind=<this host's voice-VLAN ip>;port=5062;rtp_port=12000" \
  -n <manager number>
```

Expected start-up lines:

```
Using SIP virtual modem (software Bell 212A modem, registering with SIP registrar).
SIP: Listening on 127.0.0.1:5062 (RTP port 12000), user 'mm-modem'.
SIP: Registered as mm-modem@127.0.0.1 (expires 300 s).
```

## 4. First tests

In order (ADR-0003 "Hardware checks"):

1. **SIP-2 first, if you have one:** a hardware modem on a second ATA port
   (`AT+MS=B212`, then `ATDT<manager number>`). This proves the modem path
   with known-good hardware on the calling side.
2. **SIP-1:** craft "call manager" from the terminal, or a coin-call
   call-in. No card data is involved yet.
3. If the terminal does not train: try `;answer_tone=2100`; then the DS-5
   per-terminal MODEM table ("answer tone not required"), installed once
   through a hardware modem.

Results go in the ADR-0003 hardware table and in
`docs/cardman-compat/HARDWARE_TEST_PLAN.md`.

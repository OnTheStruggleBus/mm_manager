# Running the SIP soft modem with a PhreakNet (Asterisk) server

How to answer a Millennium terminal's call-in with mm_manager's built-in
soft modem (`-S`, [ADR-0003](ADR-0003-SIP-SOFTMODEM.md)) on an Asterisk
installed by PhreakScript. Values in `<angle brackets>` are yours to choose.
Keep passwords out of this repo (SECURITY_RULES rule 8).

```
Millennium ─analog─> IAD/ATA (FXS) ─SIP/RTP─> Asterisk (PhreakNet) ─SIP/RTP, loopback─> mm_manager -S
```

**Tested 2026-11-15** on Debian 13 (x86_64, gcc 14.2) with Asterisk 22.12
(PJSIP) installed by PhreakScript, payphone lines on an IAD endpoint
(`iad01`, a Cisco IAD2432-24FXS), manager number 331-1456. A Millennium
(firmware NPA1S01 V1.0) trained on the default 2225 Hz answer, downloaded a
full install (about 40 tables in about 3¼ minutes), and made call-ins and a
maintenance call. Test-card calls were declined because no CARDMAN bridge was
running (fail closed). Results: §6 and the ADR-0003 hardware table. Not yet
tested: a hardware modem (SIP-2), a card call approved through the bridge
(SIP-5).

## 1. Build mm_manager on the Asterisk host

```bash
sudo apt-get install -y cmake build-essential git
```

```bash
git clone https://github.com/OnTheStruggleBus/mm_manager.git ~/mm_manager
```

```bash
cd ~/mm_manager && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

```bash
cd ~/mm_manager/build && ctest --output-on-failure
```

All four tests should pass. The build prints one warning from
`third-party/sqlite3.c` on GCC 14; it is expected and does not stop it.

## 2. IAD/ATA port for the payphone

Same as any modem over VoIP (SPEC §3.1):

- G.711 µ-law only. VAD, comfort noise, packet loss concealment and the
  echo canceller off. T.38 / fax re-INVITE off.
- Fixed jitter buffer, about 60 ms.
- **Polarity reversal on answer: on.** Without it the terminal shows
  Error 51.

**Cisco IAD2432 (tested).** The payphone's FXS voice port:

```
voice-port 2/20
 no echo-cancel enable
 no comfort-noise
 playout-delay mode fixed
 playout-delay nominal 100
 battery-reversal
```

- **IOS numbers the ports from 0.** `voice-port 2/20` is the 21st physical
  port, not the 20th. On the first attempts the payphone was on the port
  next to the configured one: calls came from that port's line, and training
  failed with the received level near -74 dBm0. On the configured port it
  trained at about -15.5 dBm0. Check which line the call comes from in the
  Asterisk log, then `show voice port 2/<n>`.
- On a PhreakScript IAD config, ports with a direct dial-peer (the "modem"
  and "rotary" ports) send the dialed 7 digits; touch-tone ports send the
  `1800` hotline (§4). Either reaches the manager number.
- `battery-reversal` is configured, but `show voice port 2/20 | include
  Battery` showed only `alt-battery-feed`, so whether the port reverses
  polarity is not confirmed. The terminal installed and called in without
  Error 51 anyway.

On the Asterisk side, check the IAD's endpoint already matches:

```bash
sudo asterisk -rx "pjsip show endpoint <iad endpoint>" | grep -iE "context|direct_media|allow |t38_udptl |fax_detect "
```

It should show `allow : (ulaw)`, `direct_media : false`, `fax_detect : false`
and `t38_udptl : false`.

**Answer supervision on PhreakNet.** PhreakNet answers the IAD call at
off-hook to play its own dial tone (`Answer()` in `from-<iad>`), so the IAD's
polarity reversal happens before the terminal dials, not when the modem
answers. On the tested setup the Millennium (NPA1S01 V1.0) trained and
installed anyway. If your terminal shows Error 51 or drops right after
`CONNECT`, suspect this first.

## 3. Asterisk: an endpoint for mm_manager

Keep mm_manager's Asterisk config in its own files, so PhreakScript updates
never overwrite it and it is easy to remove.

**Password.** Generate it on the host and keep it in a root-only file. It is
never typed, shown, or put on a command line:

```bash
sudo install -d -m 0700 /etc/mm_manager
```

```bash
PW=$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 40)
printf 'MM_SIP_PASSWORD=%s\n' "$PW" | sudo tee /etc/mm_manager/sip.env >/dev/null
sudo chmod 0600 /etc/mm_manager/sip.env
sudo tee /etc/asterisk/pjsip_mm.conf >/dev/null <<EOF
; mm_manager SIP soft modem (docs/softmodem/PHREAKNET.md).
[mm-modem]
type = endpoint
context = mm-no-outbound
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
password = $PW

[mm-modem]
type = aor
max_contacts = 1
remove_existing = yes
qualify_frequency = 60
EOF
unset PW
sudo chown --reference=/etc/asterisk/pjsip.conf /etc/asterisk/pjsip_mm.conf
sudo chmod 0640 /etc/asterisk/pjsip_mm.conf
```

```bash
grep -q '^#include pjsip_mm.conf' /etc/asterisk/pjsip.conf || echo '#include pjsip_mm.conf' | sudo tee -a /etc/asterisk/pjsip.conf
sudo asterisk -rx "module reload res_pjsip.so"
```

Check (do not use `| head -8`: the listing's header is 8 lines long and hides
the endpoint):

```bash
sudo asterisk -rx "pjsip show endpoints" | grep -A3 "mm-modem"
```

`mm-modem` shows **Unavailable** until mm_manager registers.

## 4. Asterisk: route the manager number

### How PhreakNet routes a call (why the obvious route does not work)

On a PhreakScript install the IAD does not send the dialed number:

1. At off-hook the IAD sends a fixed hotline number (`1800`) into its
   context (`from-iad01`). Asterisk answers and plays dial tone itself
   (`phreaknet-dialtone`, `DialTone()`), and collects the digits.
2. The collected number goes `phreaknet-internal-dest` → `phreaknet-dest` →
   `phreaknet-route`.
3. `phreaknet-route` treats the number as local only if it exists in
   **`phreaknet-local`**. Otherwise it goes out to the PhreakNet network
   over IAX2 (`to-phreaknet`).
4. A local number on the caller's own exchange goes to `phreaknet-exchange`,
   which asks **`phreaknet-peer`** for the number's device (normally
   `${HINT(<number>@phreaknet-hints)}`), then `phreaknet-ring` dials it.
   No device means intercept ("disconnected or out of service").

So an extension added to `from-<iad>` never matches, and a number that is
only in `phreaknet-local` ends at intercept. The manager number has to be in
both `phreaknet-local` and `phreaknet-peer`, which makes it an ordinary line
on the switch. Calls to it then flow like calls to any other line.

### The route

`/etc/asterisk/extensions_mm.conf`, with your manager number (7 digits, in
one of your switch's exchanges):

```bash
sudo tee /etc/asterisk/extensions_mm.conf >/dev/null <<'EOF'
; mm_manager SIP soft modem (docs/softmodem/PHREAKNET.md).
; The manager number is an ordinary line on this switch, defined like the
; switch's own lines: phreaknet-local says the number is ours, phreaknet-peer
; says which device rings.
[phreaknet-local](+)
exten => <manager number>,1,NoOp(Millennium call-in to mm_manager)
 same => n,Goto(phreaknet-exchange,${EXTEN},1)

[phreaknet-peer](+)
exten => <manager number>,1,Return(PJSIP/mm-modem)

; mm-modem's own context: it never places calls.
[mm-no-outbound]
exten => _X.,1,Hangup()
EOF
sudo chown --reference=/etc/asterisk/extensions.conf /etc/asterisk/extensions_mm.conf
sudo chmod 0640 /etc/asterisk/extensions_mm.conf
grep -q '^#include extensions_mm.conf' /etc/asterisk/extensions.conf || echo '#include extensions_mm.conf' | sudo tee -a /etc/asterisk/extensions.conf
sudo asterisk -rx "dialplan reload"
```

`[context](+)` appends to PhreakNet's context, and an exact number is matched
before PhreakNet's `_NXXXXXX` patterns. `phreaknet.conf` is not edited.

Check:

```bash
sudo asterisk -rx "dialplan show <manager number>@phreaknet-peer"
```

The first entry should be `Return(PJSIP/mm-modem)` from `extensions_mm.conf`.

**This route is open (owner decision, 2026-11-15).** Like any line on the
switch, the manager number can be called from any local line and from the
PhreakNet network, not only from the payphone. Anyone who reaches it can act
as a terminal to mm_manager; card authorizations still go through the
CARDMAN bridge, which declines anything it does not approve.

### Without PhreakNet (an ATA that sends the dialed number)

Route the number from the payphone ATA's own context instead:

```ini
[from-payphone]
exten => <manager number>,1,Dial(PJSIP/mm-modem,30)
 same => n,Congestion()

[mm-no-outbound]
exten => _X.,1,Hangup()
```

### Rules for either route (SPEC §7, ADR-0003)

- No `MixMonitor`, `Monitor`, call recording, `JITTERBUFFER()` or
  `core set debug` capture on calls to `mm-modem`: a recording of the audio
  can be decoded back into card numbers and PINs.
- No transcoding: during a call, `core show channels verbose` must show
  `ulaw` on both legs.

## 5. Run mm_manager

For a first test, run it by hand as below. For day-to-day use, run it as a
service (§5.1).

Run it from the repo directory: it reads `config/` from the current
directory and writes `mm_manager.db` there. The password goes from the
root-only file straight into its environment:

```bash
cd ~/mm_manager && MM_SIP_PASSWORD="$(sudo sed -n 's/^MM_SIP_PASSWORD=//p' /etc/mm_manager/sip.env)" \
  ./build/mm_manager -S "mm-modem@127.0.0.1:5060;port=5062;rtp_port=30000" \
  -n <manager number> -d tables/default -t tables
```

Expected:

```
Using SIP virtual modem (software Bell 212A modem, registering with SIP registrar).
SIP: Listening on 127.0.0.1:5062 (RTP port 30000), user 'mm-modem'.
Modem initialized.
Waiting for call from terminal...
SIP: Registered as mm-modem@127.0.0.1 (expires 300 s).
```

And in Asterisk:

```bash
sudo asterisk -rx "pjsip show endpoints" | grep -A3 "mm-modem"
```

`mm-modem` **Not in use**, contact `127.0.0.1:5062` **Avail**.

Notes:

- `bind` defaults to `127.0.0.1`, and only the registrar's address
  (`127.0.0.1` here) may send SIP or RTP.
- Always set `port` and `rtp_port`. mm_manager's defaults (5060, 10000)
  collide with Asterisk's SIP port and its default RTP range 10000-20000
  (`rtp.conf`).
- If Asterisk sends media from a different address than its SIP address
  (`media_address`, several interfaces), RTP from the address in its SDP is
  accepted too.
- `CARDMAN_BRIDGE_ADDR` defaults to `127.0.0.1:8420`. With no bridge
  running, every card call is declined (fail closed).
- Do not add `-v`, `-l`, `-p` or `-u`: they print or save raw packet data,
  which includes card numbers and PINs (rule 4, task T4).
- One call at a time. For a second simultaneous call-in, run another
  instance with its own SIP user, ports and working directory.

If Asterisk is on another host, put both on an isolated voice VLAN and bind
to it explicitly (mm_manager refuses to start with a remote registrar and
the default loopback bind):

```bash
./build/mm_manager -S "mm-modem@<asterisk ip>:5060;bind=<this host's voice-VLAN ip>;port=5062;rtp_port=30000" -n <manager number>
```

### 5.1 Run it as a service (systemd)

Tested on the same host. mm_manager runs as its own unprivileged user, from
`/var/lib/mm-manager`, and starts after Asterisk at boot. Stop any copy you
started by hand first: two instances clash on port 5062.

```bash
sudo useradd --system --home-dir /var/lib/mm-manager --shell /usr/sbin/nologin mm-manager
```

```bash
sudo install -d -o mm-manager -g mm-manager -m 0750 /var/lib/mm-manager
```

```bash
sudo cp -a ~/mm_manager/config ~/mm_manager/tables /var/lib/mm-manager/
```

If you already ran mm_manager by hand, move its database too, so the
terminals' records carry over:

```bash
sudo mv ~/mm_manager/mm_manager.db /var/lib/mm-manager/
```

```bash
sudo chown -R mm-manager:mm-manager /var/lib/mm-manager
```

```bash
sudo install -m 0755 ~/mm_manager/build/mm_manager /usr/local/bin/mm_manager
```

`/etc/systemd/system/mm-manager.service` (systemd reads `sip.env` as root
and passes the password in the environment; the service user cannot read the
file):

```ini
[Unit]
Description=mm_manager - Nortel Millennium manager (SIP soft modem)
After=network-online.target asterisk.service
Wants=network-online.target

[Service]
Type=simple
User=mm-manager
Group=mm-manager
WorkingDirectory=/var/lib/mm-manager
EnvironmentFile=/etc/mm_manager/sip.env
ExecStart=/usr/bin/stdbuf -oL -eL /usr/local/bin/mm_manager -S "mm-modem@127.0.0.1:5060;port=5062;rtp_port=30000" -n <manager number> -d tables/default -t tables
KillSignal=SIGINT
TimeoutStopSec=15
Restart=always
RestartSec=5
LimitCORE=0
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/var/lib/mm-manager
PrivateTmp=yes
PrivateDevices=yes

[Install]
WantedBy=multi-user.target
```

- `stdbuf -oL` makes mm_manager's output reach the journal line by line.
- `LimitCORE=0`: a core dump would contain card data from memory.
- `KillSignal=SIGINT` stops it the same way as Ctrl-C.

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now mm-manager
```

```bash
systemctl status mm-manager
```

Check it registered (`mm-modem` **Not in use**, contact **Avail**) as in §5.
Watch calls with `journalctl -u mm-manager -f`.

Day to day:

- The live tables and database are in `/var/lib/mm-manager`; the copies in
  `~/mm_manager` are no longer used. Files added there must be owned by
  `mm-manager`.
- To update: `git pull` and rebuild in `~/mm_manager`, then
  `sudo install -m 0755 ~/mm_manager/build/mm_manager /usr/local/bin/mm_manager && sudo systemctl restart mm-manager`.
- The journal holds what mm_manager prints, with card numbers masked. Never
  add `-v`, `-l`, `-p` or `-u` to `ExecStart` (rule 4).

## 6. First tests

**Voice-call check (no modem).** From any phone on the IAD, dial the manager
number. You hear ringback, about 2 s of silence, the steady 2225 Hz answer
tone, then hiss. mm_manager prints:

```
SIP: Incoming call from sip:<line>@... (PCMU).
SIP: Call answered.
Virtual modem: answering, Answer Silence.
Virtual modem: Answer Tone (rx level ... dBm0)
Virtual modem: Training (rx level ... dBm0)
SIP: Call ended: remote hangup.
... Carrier lost.
```

A voice phone cannot train, so the call ends there. That is the expected
result: the dialplan, SIP registration and audio path all work.

Then, in order (ADR-0003 "Hardware checks"):

1. **SIP-2, if you have one:** a hardware modem on another IAD port
   (`AT+MS=B212`, then `ATDT<manager number>`). It should reach
   `Virtual modem: Data` and `CONNECT`.
2. **SIP-1:** key the manager number into the terminal, then craft "call
   manager". No card data is involved.
3. If the terminal does not train: try `;answer_tone=2100`; then the DS-5
   per-terminal MODEM table ("answer tone not required"), installed once
   through a hardware modem.

Results go in the ADR-0003 hardware table and in
`docs/cardman-compat/HARDWARE_TEST_PLAN.md`.

### What happened on the tested setup (2026-11-15)

- **Terminal setup.** The terminal asks for the manager number with an area
  code (full NPA). We keyed `216 331 1456`; PhreakNet routes on the 7
  digits. The terminal's own ID was set in the same NPA (`2163311821`).
- **Training.** Trained on the default 2225 Hz answer and default MODEM
  table; no `answer_tone=2100` or DS-5 table was needed. Received level about
  -15.5 dBm0. (Near -74 dBm0 means the payphone is on the wrong IAD port, §2.)
- **Install.** The first full install ended with the terminal showing
  "download failed" although mm_manager logged the session as complete. The
  next call-in finished the install (status `02`). The upstream README
  reports the same with old terminal firmware (this one is NPA1S01 V1.0).
  If the first install shows failed, let it call in once more.
- **Call-ins and maintenance.** Later call-ins and a maintenance call
  completed normally.
- **Card calls.** Test-card calls reached mm_manager and were declined,
  because no CARDMAN bridge was running (`could not reach bridge … failing
  closed`). The card number and PIN appeared only masked in mm_manager's
  output. This was checked by eye, not by the rule 9 decode audit (SIP-6).

## 7. Reading the Asterisk log

Watch a call with `sudo asterisk -rvvv`.

| In the log | Meaning | Fix |
|---|---|---|
| `1800@from-<iad>` then `DialTone(...)`, and your `from-<iad>` extension never runs | PhreakNet collects the digits itself | Route through `phreaknet-local` and `phreaknet-peer` (§4) |
| `to-phreaknet` … `IAX2/...` … `is circuit-busy` | The number is not in `phreaknet-local`, so it went to the network | Add it to `phreaknet-local` |
| `phreaknet-peer` … `Return("")` then `phreaknet-intercept` ("disconnected or out of service") | No device for the number | Add it to `phreaknet-peer` |
| `phreaknet-ring` … `Dial(PJSIP/mm-modem,...)` … `PJSIP/mm-modem-… answered` | Routed and answered | — |
| `Spawn extension (mm-no-outbound, s, 1) exited non-zero on 'PJSIP/mm-modem-…'` | PhreakNet's pre-dial step ran on the modem's channel | Harmless; the call continues |
| `File custom/dialcomplete/complete1 does not exist` | A sound file missing from the PhreakNet install | Unrelated to mm_manager |

#!/usr/bin/env python3
"""
Reference CARDMAN bridge for the mm_manager fork (task T16).

Listens on loopback for mm_manager's POST /authorize and POST /cdr
(src/mm_cardman_bridge.c; hop contract in
docs/cardman-compat/BRIDGE_CONTRACT.md) and turns them into CARDMAN's
CAUT (POST /api/card/authorize) and CDRI (POST /api/cdr) calls, as
specified by MCardSys docs/frontends/MILLENNIUM_GATEWAY_CONTRACT.md
(CARDMAN 2.5.0.1, ADR 40).

Identity: one `payphone` WAPI identity per deployment, mTLS client cert
with CN mm-<env> plus HTTP Basic-Auth with the same userid. It may call
CAUT and CDRI only. Credentials come from the command line / environment,
never from this repo (AI/SECURITY_RULES.md rule 8).

Fail closed: anything but a clear CAUT `approved` is a decline at the
terminal. `approved_limited` is declined too, and its hold is given back
at once with a zero-second release CDR, because a payphone has no void
route and CARDMAN never expires an open hold.

Structure follows the integration repo's bridge
(OnTheStruggleBus/-mm-cardman-integration, bridge/cardman_bridge.py);
rewritten here for the 2.5.0.1 contract.
"""
import argparse
import base64
import hashlib
import hmac
import http.client
import http.server
import ipaddress
import json
import os
import re
import secrets
import socketserver
import ssl
import sys
import threading
import time
import urllib.parse

DEFAULT_LISTEN = "127.0.0.1:8420"
CAUT_PATH = "/api/card/authorize"
CDRI_PATH = "/api/cdr"
# Per-attempt timeouts: CARDMAN's route timeouts (CAUT 3 s, CDRI 5 s)
# plus TLS overhead. One retry of an identical body fits inside
# mm_manager's 10 s client timeout.
CAUT_TIMEOUT = 4.0
CDRI_TIMEOUT = 4.5
REQUEST_BUDGET = 9.0
REQUESTED_AT_TTL = 600

TOLL_FREE_NPAS = {"800", "833", "844", "855", "866", "877", "888"}
# CALLTYP low nibble (src/mm_manager.h).
CALL_TYPE_UNANSWERED = 0x01
CALL_TYPE_ABANDONED = 0x02
CALL_TYPE_INTERNATIONAL = 0x06
CALL_TYPE_DENIED = 0x0B
NOT_CONNECTED = {CALL_TYPE_UNANSWERED, CALL_TYPE_ABANDONED, CALL_TYPE_DENIED}

CDRI_STATES = {"MATCHED", "LATE", "FILTERED", "UNMATCHED", "DUPLICATE", "MERGED", "NOTARIFF"}

AUTH_CODE_RE = re.compile(r"^[0-9A-Z]{8}$")
ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
DIGITS_RE = re.compile(r"^\d+$")
CDR_ID_RE = re.compile(r'^[^"\\]{1,128}$')


def log(msg):
    print(f"[cardman_bridge] {msg}", file=sys.stderr, flush=True)


def mask_pan(pan):
    pan = str(pan or "")
    return pan[:6] + "****" + pan[-4:] if len(pan) >= 10 else "****"


# ---- pure mapping helpers (unit-tested in tools/test_cardman_bridge.py) ----

def normalize_number(dialed, terminal_phone, international):
    """Shapes a dialed number the way CAUT and CDRI rate it.

    CDRI has no international flag: 10 digits or 1+10 is domestic (or
    toll-free by NPA), anything else is INTL. So NANP numbers must be
    10 or 1+10 digits; a 7-digit local number takes the payphone's own
    NPA. International numbers are left as dialed.
    """
    d = re.sub(r"\D", "", dialed or "")
    if international or d.startswith("011"):
        return d
    npa = re.sub(r"\D", "", terminal_phone or "")[-10:][:3]
    if len(d) == 7 and len(npa) == 3:
        return npa + d
    if len(d) == 8 and d[0] in "01" and len(npa) == 3:   # 0/1 + 7 digits
        return npa + d[1:]
    if len(d) == 11 and d[0] == "0":                    # 0 + 10 (operator/card dialing)
        return d[1:]
    return d


def is_toll_free(number):
    if len(number) == 11 and number[0] == "1":
        return number[1:4] in TOLL_FREE_NPAS
    if len(number) == 10:
        return number[:3] in TOLL_FREE_NPAS
    return False


def terminal_verdict(caut_status, caut_body):
    """Maps a CAUT exchange to (approved, disposition, auth_code).

    Only HTTP 200 + disposition 'approved' + a well-formed auth_code is
    valid. Everything else, approved_limited included, is invalid.
    """
    if caut_status != 200 or not isinstance(caut_body, dict):
        return False, None, None
    disposition = caut_body.get("disposition")
    auth_code = caut_body.get("auth_code") or caut_body.get("transaction_ref") or ""
    if disposition == "approved" and AUTH_CODE_RE.match(str(auth_code)):
        return True, disposition, auth_code
    return False, disposition, auth_code if AUTH_CODE_RE.match(str(auth_code)) else None


def cdr_charge_cents(req):
    """Reported (not billed, ADR 8) charge: the terminal's own cost, or 0."""
    for key in ("call_cost_requested", "call_cost_collected"):
        v = req.get(key)
        if isinstance(v, int) and v > 0:
            return v
    return 0


def cdr_duration_secs(req):
    """Answered seconds; 0 for a call that never connected, so CDRI
    FILTERs it and releases the hold instead of billing ring time."""
    call_type = req.get("call_type")
    if isinstance(call_type, int) and (call_type & 0x0F) in NOT_CONNECTED:
        return 0
    d = req.get("duration_secs")
    return d if isinstance(d, int) and d >= 0 else 0


def cdri_delivered(status, body):
    """True when CARDMAN now owns the record (any recognised state).
    DUPLICATE is success: recorded_state holds the original outcome.
    Transport errors, INTERNAL and other shapes are retryable."""
    if not isinstance(body, dict):
        return False
    if status == 200 and body.get("state") in CDRI_STATES:
        return True
    return False


class RequestedAtCache:
    """Keeps requested_at stable per (terminal, seq, card) so a retry
    places no second hold even if mm_manager restarted in between. The
    card is keyed by an HMAC under a per-process random key, so nothing
    here can be reversed to a PAN, and it lives in memory only."""

    def __init__(self, ttl=REQUESTED_AT_TTL):
        self.ttl = ttl
        self.key = secrets.token_bytes(32)
        self.lock = threading.Lock()
        self.items = {}

    def stable(self, terminal_id, seq_num, pan, requested_at, now=None):
        now = time.time() if now is None else now
        tag = hmac.new(self.key, pan.encode(), hashlib.sha256).hexdigest()
        k = (terminal_id, seq_num, tag)
        with self.lock:
            for key in [k2 for k2, (_, t) in self.items.items() if now - t >= self.ttl]:
                del self.items[key]
            if k in self.items:
                return self.items[k][0]
            self.items[k] = (requested_at, now)
            return requested_at


# ---- CARDMAN client -------------------------------------------------------

class CardmanError(Exception):
    pass


class CardmanClient:
    """mTLS + Basic-Auth WAPI client. http:// is for the test mock only."""

    def __init__(self, base_url, cert=None, key=None, ca=None, user=None, password=None):
        u = urllib.parse.urlsplit(base_url)
        if u.scheme not in ("http", "https"):
            raise ValueError(f"unsupported scheme in {base_url}")
        self.scheme, self.host = u.scheme, u.hostname
        self.port = u.port or (443 if u.scheme == "https" else 80)
        self.auth = None
        if user is not None:
            self.auth = "Basic " + base64.b64encode(f"{user}:{password or ''}".encode()).decode()
        self.ctx = None
        if self.scheme == "https":
            self.ctx = ssl.create_default_context(cafile=ca)
            self.ctx.load_cert_chain(certfile=cert, keyfile=key)

    def post(self, path, payload, timeout):
        """Returns (status, parsed JSON or None). Raises CardmanError on a
        transport failure."""
        body = json.dumps(payload, separators=(",", ":")).encode()
        headers = {"Content-Type": "application/json"}
        if self.auth:
            headers["Authorization"] = self.auth
        if self.scheme == "https":
            conn = http.client.HTTPSConnection(self.host, self.port, timeout=timeout, context=self.ctx)
        else:
            conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        try:
            conn.request("POST", path, body=body, headers=headers)
            resp = conn.getresponse()
            raw = resp.read()
        except (OSError, http.client.HTTPException) as exc:
            raise CardmanError(f"{path}: {exc}") from exc
        finally:
            conn.close()
        try:
            return resp.status, json.loads(raw)
        except (ValueError, UnicodeDecodeError):
            return resp.status, None

    def post_with_retry(self, path, payload, timeout, budget=REQUEST_BUDGET):
        """Sends the identical body again once after a transport error,
        if time allows. Safe for both routes: CAUT is idempotent on
        (pan, seq_num, requested_at), CDRI on cdr_id."""
        deadline = time.monotonic() + budget
        last = None
        for _ in range(2):
            left = deadline - time.monotonic()
            if left < 1.0:
                break
            try:
                return self.post(path, payload, min(timeout, left))
            except CardmanError as exc:
                last = exc
                log(f"{path}: attempt failed ({exc})")
        raise last or CardmanError(f"{path}: no time left")


# ---- the bridge -----------------------------------------------------------

class Bridge:
    def __init__(self, client, telco_id="", requested_at_cache=None):
        self.client = client
        self.telco_id = telco_id
        self.cache = requested_at_cache or RequestedAtCache()

    def caut_body(self, req):
        terminal_id = str(req["terminal_id"])
        phone = str(req.get("phone_number") or terminal_id)
        international = req.get("international") == "yes" or (
            isinstance(req.get("call_type"), int) and (req["call_type"] & 0x0F) == CALL_TYPE_INTERNATIONAL)
        called = normalize_number(req.get("called_number", ""), phone, international)
        requested_at = self.cache.stable(terminal_id, req["seq_num"], req["pan"], req["requested_at"])
        return {
            "variant": "funf",                      # the terminal collects a PIN
            "pan": req["pan"],
            "service_code": str(req.get("service_code", "")),
            "pin": req["pin"],
            "called_number": called,
            # Only "yes" selects INTL. A toll-free NPA wins either way
            # inside CAUT, so this is never sent as "yes" for one.
            "international": "yes" if international and not is_toll_free(called) else "no",
            "seq_num": req["seq_num"],
            "requested_at": requested_at,
            "terminal": {                           # accepted, not stored (2.5.0.1)
                "telco_id": self.telco_id,
                "terminal_id": terminal_id,
                "phone_number": phone,
                "terminal_serial": "",
            },
        }

    def release_cdr(self, caut, auth_code):
        """Zero-second CDR: CDRI answers FILTERED and releases the hold.
        cdr_id is stable for the same auth, so a resend is a DUPLICATE."""
        t = caut["terminal"]
        return {
            "cdr_id": f"mm-{t['terminal_id']}-A{caut['seq_num']}-{caut['requested_at']}",
            "auth_code": auth_code,
            "pan": caut["pan"],
            "charge_cents": 0,
            "start_ts": caut["requested_at"],
            "duration_secs": 0,
            "called_number": caut["called_number"],
            "ani": t["phone_number"],
            "dnis": caut["called_number"],
            "switch_id": t["terminal_id"],
            "source": "mm_manager",
        }

    def send_release(self, caut, auth_code, why):
        body = self.release_cdr(caut, auth_code)
        try:
            status, resp = self.client.post_with_retry(CDRI_PATH, body, CDRI_TIMEOUT)
        except CardmanError as exc:
            status, resp = None, None
            log(f"release: CDRI failed ({exc})")
        state = resp.get("state") if isinstance(resp, dict) else None
        log(f"release ({why}): cdr_id={body['cdr_id']} auth_code={auth_code} state={state} http={status}")
        if not cdri_delivered(status, resp):
            log(f"ALERT: hold {auth_code} for card {mask_pan(caut['pan'])} is still OPEN; "
                f"resend cdr_id={body['cdr_id']} or release it in CARDMAN")
        return state

    def authorize(self, req):
        """Returns the response for mm_manager and, when a hold must be
        given back, the CAUT body and auth_code to release."""
        if not validate_authorize(req):
            log("authorize: request failed validation, declining")
            return {"approved": False, "disposition": None}, None
        caut = self.caut_body(req)
        try:
            status, resp = self.client.post_with_retry(CAUT_PATH, caut, CAUT_TIMEOUT)
        except CardmanError as exc:
            log(f"authorize: CAUT failed ({exc}), declining")
            return {"approved": False, "disposition": None}, None

        approved, disposition, auth_code = terminal_verdict(status, resp)
        log(f"authorize: terminal={caut['terminal']['terminal_id']} seq={caut['seq_num']} "
            f"card={mask_pan(caut['pan'])} http={status} disposition={disposition} "
            f"auth_code={auth_code} -> {'valid' if approved else 'invalid'}")
        if approved:
            return {"approved": True, "disposition": disposition, "auth_code": auth_code}, (caut, auth_code)
        if disposition == "approved_limited" and auth_code:
            self.send_release(caut, auth_code, "approved_limited")
        return {"approved": False, "disposition": disposition}, None

    def cdr(self, req):
        if not validate_cdr(req):
            log("cdr: request failed validation, not sent")
            return {"delivered": False, "retryable": False}
        terminal_id = str(req["terminal_id"])
        phone = str(req.get("phone_number") or terminal_id)
        international = isinstance(req.get("call_type"), int) and (req["call_type"] & 0x0F) == CALL_TYPE_INTERNATIONAL
        called = normalize_number(req.get("called_number", ""), phone, international)
        body = {
            "cdr_id": req["cdr_id"],
            "auth_code": req["auth_code"],
            "pan": req["pan"],
            "charge_cents": cdr_charge_cents(req),
            "start_ts": req["start_ts"],
            "duration_secs": cdr_duration_secs(req),
            "called_number": called,
            "ani": phone,
            "dnis": called,
            "switch_id": terminal_id,
            "source": "mm_manager",
        }
        try:
            status, resp = self.client.post_with_retry(CDRI_PATH, body, CDRI_TIMEOUT)
        except CardmanError as exc:
            log(f"cdr: cdr_id={body['cdr_id']} CDRI failed ({exc}); not delivered")
            return {"delivered": False, "retryable": True}
        delivered = cdri_delivered(status, resp)
        state = resp.get("state") if isinstance(resp, dict) else None
        log(f"cdr: cdr_id={body['cdr_id']} auth_code={body['auth_code']} card={mask_pan(body['pan'])} "
            f"duration={body['duration_secs']} http={status} state={state} "
            f"recorded_state={(resp or {}).get('recorded_state')} charged={(resp or {}).get('charged_cents')}")
        out = {"delivered": delivered, "state": state}
        if isinstance(resp, dict):
            for k in ("recorded_state", "charged_cents"):
                if k in resp:
                    out[k] = resp[k]
        if not delivered:
            out["retryable"] = status != 400
        return out


def validate_authorize(req):
    try:
        return bool(DIGITS_RE.match(req["pan"]) and len(req["pan"]) == 16
                    and DIGITS_RE.match(req["pin"]) and len(req["pin"]) == 4
                    and DIGITS_RE.match(str(req["terminal_id"]))
                    and (req.get("called_number", "") == "" or DIGITS_RE.match(req["called_number"]))
                    and isinstance(req["seq_num"], int) and 0 <= req["seq_num"] <= 65535
                    and ISO_RE.match(req["requested_at"]))
    except (KeyError, TypeError):
        return False


def validate_cdr(req):
    try:
        return bool(CDR_ID_RE.match(req["cdr_id"]) and AUTH_CODE_RE.match(req["auth_code"])
                    and DIGITS_RE.match(req["pan"]) and len(req["pan"]) == 16
                    and DIGITS_RE.match(str(req["terminal_id"]))
                    and ISO_RE.match(req["start_ts"]))
    except (KeyError, TypeError):
        return False


class Handler(http.server.BaseHTTPRequestHandler):
    def _body(self):
        try:
            n = int(self.headers.get("Content-Length", 0))
            return json.loads(self.rfile.read(n)) if 0 < n <= 4096 else None
        except (ValueError, UnicodeDecodeError):
            return None

    def _send(self, payload):
        data = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        self.wfile.flush()

    def do_POST(self):
        req = self._body()
        bridge = self.server.bridge
        if self.path == "/authorize":
            resp, hold = bridge.authorize(req) if isinstance(req, dict) else ({"approved": False}, None)
            try:
                self._send(resp)
            except OSError as exc:
                # mm_manager will decline (fail closed), so the hold just
                # placed would never get a CDR. Give it back now.
                if hold:
                    bridge.send_release(hold[0], hold[1], f"response to mm_manager lost: {exc}")
        elif self.path == "/cdr":
            self._send(bridge.cdr(req) if isinstance(req, dict) else {"delivered": False, "retryable": False})
        else:
            self.send_error(404)

    def log_message(self, fmt, *args):
        pass    # explicit, masked log() lines only


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def main(argv=None):
    p = argparse.ArgumentParser(description="Reference CARDMAN bridge for mm_manager (payphone identity).")
    p.add_argument("--listen", default=DEFAULT_LISTEN, help="loopback host:port (default %(default)s)")
    p.add_argument("--cardman-url", required=True, help="e.g. https://127.0.0.1:8443")
    p.add_argument("--client-cert", help="payphone client cert, CN mm-<env>")
    p.add_argument("--client-key")
    p.add_argument("--ca-cert", help="CA that signed CARDMAN's server cert")
    p.add_argument("--user", default=os.environ.get("CARDMAN_BRIDGE_USER"),
                   help="Basic-Auth userid, same as the cert CN (or CARDMAN_BRIDGE_USER)")
    p.add_argument("--telco-id", default=os.environ.get("CARDMAN_TELCO_ID", ""),
                   help="telco_id for CAUT's terminal object")
    a = p.parse_args(argv)
    password = os.environ.get("CARDMAN_BRIDGE_PASSWORD")     # env only, never argv

    if a.cardman_url.startswith("https://"):
        if not (a.client_cert and a.client_key and a.ca_cert and a.user and password):
            p.error("https needs --client-cert, --client-key, --ca-cert, --user and CARDMAN_BRIDGE_PASSWORD")
    host, port = a.listen.rsplit(":", 1)
    if not ipaddress.ip_address(host).is_loopback:
        p.error("--listen must be a loopback address: card data must not leave this host unencrypted")

    client = CardmanClient(a.cardman_url, a.client_cert, a.client_key, a.ca_cert, a.user, password)
    server = Server((host, int(port)), Handler)
    server.bridge = Bridge(client, telco_id=a.telco_id)
    log(f"listening on {host}:{port} -> {a.cardman_url} as {a.user}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

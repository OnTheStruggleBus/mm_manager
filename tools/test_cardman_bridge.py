#!/usr/bin/env python3
"""
Tests for tools/cardman_bridge.py against a mock CARDMAN.

The mock follows MCardSys 2.5.0.1 where it matters for the bridge:
CAUT's required seq_num/requested_at (400 without), auth_code derived
by a port of auth.rexx DERIVE_AC, one hold per auth_code, the
approved_limited cap for high-risk NPAs (900, 976), CDRI's cdr_id
dedupe (DUPLICATE + recorded_state), the under-3-second FILTERED release,
UNMATCHED without a hold, and call type derived from the called number's
shape. It is not CARDMAN: the live check is tools/cardman_live_test.sh.

Run:  python3 tools/test_cardman_bridge.py
With MM_CARDMAN_DRIVE=<build>/mm_cardman_drive set, the end-to-end cases
also drive the fork's C glue (src/mm_cardman.c) through the bridge.
"""
import base64
import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cardman_bridge as cb  # noqa: E402

TEST_PAN = "9149000000000011"      # synthetic test PAN, never a real card
TEST_PIN = "2468"                  # dummy PIN (AI/SECURITY_RULES.md rule 3)
TERMINAL = "2125551000"
B36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def derive_ac(pan, seq_num, requested_at):
    """Port of MCardSys bricks/runtime/rexx/auth.rexx DERIVE_AC."""
    h1, h2 = 5381, 52711
    for ch in f"{pan}|{seq_num}|{requested_at}".encode():
        h1 = (h1 * 33 + ch) % 2176782336
        h2 = (h2 * 31 + ch) % 1296
    code = ""
    for _ in range(6):
        code = B36[h1 % 36] + code
        h1 //= 36
    for _ in range(2):
        code = B36[h2 % 36] + code
        h2 //= 36
    return code


def derive_calltype(cn):
    """Port of cdrin.rexx DERIVE_CALLTYPE."""
    if len(cn) == 11 and cn[0] == "1":
        npa = cn[1:4]
    elif len(cn) == 10:
        npa = cn[:3]
    else:
        return "INTL"
    return "TOLLFREE" if npa in cb.TOLL_FREE_NPAS else "DOM"


class MockCardman:
    USER, PASSWORD = "mm-dev", "mock-only-not-a-cardman-password"

    def __init__(self):
        self.balance = 2000
        self.holds = {}        # auth_code -> {"amount", "state", "call_type"}
        self.cdrs = {}         # cdr_id -> row
        self.caut_bodies = []
        self.fail_next_caut = 0
        self.lock = threading.Lock()

    def caut(self, b):
        self.caut_bodies.append(b)
        if "seq_num" not in b or not b.get("requested_at"):
            return 400, {"ok": False, "error": "seq_num and requested_at are required", "code": "BAD_REQUEST"}
        cn = b.get("called_number", "")
        npa = cn[1:4] if len(cn) == 11 and cn[0] == "1" else cn[:3] if len(cn) == 10 else ""
        if npa in cb.TOLL_FREE_NPAS:
            call_type = "TOLLFREE"
        else:
            call_type = "INTL" if b.get("international") == "yes" else "DOM"
        if b.get("pin") != TEST_PIN or b.get("variant") != "funf":
            return 200, {"ok": True, "disposition": "declined", "auth_code": "", "transaction_ref": ""}
        ac = derive_ac(b["pan"], b["seq_num"], b["requested_at"])
        limited = npa in ("900", "976")
        want = 150 if limited else min(300 if call_type != "INTL" else 900, 2000)
        if ac not in self.holds:
            held = sum(h["amount"] for h in self.holds.values() if h["state"] == "OPEN")
            if self.balance - held < want:
                return 200, {"ok": True, "disposition": "declined", "auth_code": "", "transaction_ref": ""}
            self.holds[ac] = {"amount": want, "state": "OPEN", "call_type": call_type}
        disp = "approved_limited" if limited else "approved"
        return 200, {"ok": True, "disposition": disp, "auth_code": ac, "transaction_ref": ac}

    def cdri(self, b):
        if not isinstance(b.get("charge_cents"), int) or b["charge_cents"] < 0:
            return 400, {"ok": False, "error": "charge_cents", "code": "BAD_REQUEST"}
        cid = b["cdr_id"]
        if cid in self.cdrs:
            row = self.cdrs[cid]
            return 200, {"ok": True, "cdr_id": cid, "state": "DUPLICATE",
                         "charged_cents": 0, "recorded_state": row["state"]}
        hold = self.holds.get(b.get("auth_code", ""))
        row = dict(b, call_type=derive_calltype(b.get("called_number", "")), charged=0)
        if hold is None:
            row["state"] = "UNMATCHED"
        elif b["duration_secs"] < 3:
            row["state"] = "FILTERED"
            if hold["state"] == "OPEN":
                hold["state"] = "RELEASED"
        elif hold["state"] == "OPEN":
            rated = 100 * -(-b["duration_secs"] // 60)
            row["charged"] = min(rated, hold["amount"])
            row["state"] = "MATCHED"
            hold["state"] = "CAPTURED"
            self.balance -= row["charged"]
        else:
            row["state"] = "LATE"
        self.cdrs[cid] = row
        return 200, {"ok": True, "cdr_id": cid, "state": row["state"],
                     "charged_cents": row["charged"], "recorded_state": row["state"]}


class MockHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        m = self.server.mock
        expect = "Basic " + base64.b64encode(f"{m.USER}:{m.PASSWORD}".encode()).decode()
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with m.lock:
            if self.headers.get("Authorization") != expect:
                status, resp = 401, {"ok": False}
            elif self.path == cb.CAUT_PATH and m.fail_next_caut > 0:
                m.fail_next_caut -= 1
                self.close_connection = True
                return            # drop the connection: a transport error
            elif self.path == cb.CAUT_PATH:
                status, resp = m.caut(body)
            elif self.path == cb.CDRI_PATH:
                status, resp = m.cdri(body)
            else:
                status, resp = 403, {"ok": False}
        data = json.dumps(resp).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


class ThreadedServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def serve(handler, attr, value):
    srv = ThreadedServer(("127.0.0.1", 0), handler)
    setattr(srv, attr, value)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


class Pure(unittest.TestCase):
    def test_normalize(self):
        n = cb.normalize_number
        self.assertEqual(n("5551212", TERMINAL, False), "2125551212")         # 7-digit local
        self.assertEqual(n("15551212", TERMINAL, False), "2125551212")
        self.assertEqual(n("3125551212", TERMINAL, False), "3125551212")
        self.assertEqual(n("13125551212", TERMINAL, False), "13125551212")
        self.assertEqual(n("03125551212", TERMINAL, False), "3125551212")     # 0+ card dialing
        self.assertEqual(n("011442071234567", TERMINAL, True), "011442071234567")
        self.assertEqual(n("011442071234567", TERMINAL, False), "011442071234567")

    def test_verdict(self):
        v = cb.terminal_verdict
        self.assertEqual(v(200, {"disposition": "approved", "auth_code": "A1B2C3D4"})[0], True)
        for d in ("approved_limited", "declined", "bad_card", "requires_card", "bad_country", "no_tariff"):
            self.assertFalse(v(200, {"disposition": d, "auth_code": "A1B2C3D4"})[0], d)
        self.assertFalse(v(403, {"disposition": "approved", "auth_code": "A1B2C3D4"})[0])
        self.assertFalse(v(200, None)[0])
        for bad in ("bogus", "", "a1b2c3d4", "A1B2C3D4X", "A1B2C3D"):
            self.assertFalse(v(200, {"disposition": "approved", "auth_code": bad})[0], bad)
        # transaction_ref is not a substitute for a missing auth_code.
        self.assertFalse(v(200, {"disposition": "approved", "transaction_ref": "A1B2C3D4"})[0])
        self.assertFalse(v(200, {"disposition": "approved", "auth_code": "", "transaction_ref": "A1B2C3D4"})[0])
        self.assertFalse(v(200, {"disposition": "approved", "auth_code": None, "transaction_ref": "A1B2C3D4"})[0])

    def test_http_only_to_loopback(self):
        for url in ("http://127.0.0.1:8443", "http://localhost:8443", "http://[::1]:8443"):
            cb.CardmanClient(url)
        for url in ("http://192.0.2.1:8443", "http://10.0.0.1", "http://[2001:db8::1]:8443", "ftp://127.0.0.1"):
            with self.assertRaises(ValueError, msg=url):
                cb.CardmanClient(url)

    def test_cdr_fields(self):
        self.assertEqual(cb.cdr_duration_secs({"call_type": 0x51, "duration_secs": 25}), 0)
        self.assertEqual(cb.cdr_duration_secs({"call_type": 0x52, "duration_secs": 9}), 0)
        self.assertEqual(cb.cdr_duration_secs({"call_type": 0x55, "duration_secs": 95}), 95)
        self.assertEqual(cb.cdr_charge_cents({"call_cost_requested": 0, "call_cost_collected": 0}), 0)
        self.assertEqual(cb.cdr_charge_cents({"call_cost_requested": 250}), 250)
        self.assertTrue(cb.cdri_delivered(200, {"state": "DUPLICATE", "recorded_state": "MATCHED"}))
        self.assertTrue(cb.cdri_delivered(200, {"ok": False, "code": "NO_TARIFF", "state": "NOTARIFF"}))
        self.assertFalse(cb.cdri_delivered(200, {"ok": False, "code": "INTERNAL"}))
        self.assertFalse(cb.cdri_delivered(400, {"ok": False, "code": "BAD_REQUEST"}))

    def test_requested_at_cache(self):
        c = cb.RequestedAtCache(ttl=600)
        a = c.stable(TERMINAL, 7, TEST_PAN, "2026-10-13T18:42:55Z", now=1000)
        self.assertEqual(c.stable(TERMINAL, 7, TEST_PAN, "2026-10-13T18:43:30Z", now=1035), a)
        self.assertEqual(c.stable(TERMINAL, 7, "9149000000000029", "2026-10-13T18:43:30Z", now=1035),
                         "2026-10-13T18:43:30Z")
        self.assertEqual(c.stable(TERMINAL, 7, TEST_PAN, "2026-10-13T19:00:00Z", now=1700), "2026-10-13T19:00:00Z")
        self.assertNotIn(TEST_PAN, repr(c.items))

    def test_derive_ac_shape(self):
        self.assertRegex(derive_ac(TEST_PAN, 65535, "2026-10-13T18:42:55Z"), r"^[0-9A-Z]{8}$")


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.mock = MockCardman()
        self.cm = serve(MockHandler, "mock", self.mock)
        client = cb.CardmanClient(f"http://127.0.0.1:{self.cm.server_port}",
                                  user=MockCardman.USER, password=MockCardman.PASSWORD)
        self.bridge = cb.Bridge(client, telco_id="TELCO01")
        self.br = serve(cb.Handler, "bridge", self.bridge)
        self.addr = f"127.0.0.1:{self.br.server_port}"

    def tearDown(self):
        self.br.shutdown()
        self.cm.shutdown()

    def post(self, path, body):
        import http.client
        c = http.client.HTTPConnection("127.0.0.1", self.br.server_port, timeout=15)
        c.request("POST", path, json.dumps(body), {"Content-Type": "application/json"})
        r = c.getresponse()
        return json.loads(r.read())

    def auth(self, seq, dialed, call_type=0x55, requested_at="2026-10-13T18:42:55Z"):
        return self.post("/authorize", {
            "pan": TEST_PAN, "pin": TEST_PIN, "service_code": "101", "called_number": dialed,
            "terminal_id": TERMINAL, "phone_number": TERMINAL, "seq_num": seq,
            "requested_at": requested_at, "call_type": call_type,
            "international": "yes" if call_type & 0x0F == 6 else "no"})

    def cdr(self, seq, ac, dialed, secs, call_type=0x55, start="2026-10-13T18:43:00Z"):
        return self.post("/cdr", {
            "cdr_id": f"mm-{TERMINAL}-{seq}-{start}", "auth_code": ac, "pan": TEST_PAN,
            "called_number": dialed, "start_ts": start, "duration_secs": secs,
            "call_type": call_type, "rate_type": 0, "call_cost_collected": 0,
            "call_cost_requested": 175, "terminal_id": TERMINAL, "phone_number": TERMINAL, "seq": seq})

    def test_answered_call(self):
        a = self.auth(1, "3125551212")
        self.assertTrue(a["approved"])
        c = self.cdr(10, a["auth_code"], "3125551212", 95)
        self.assertEqual((c["delivered"], c["state"]), (True, "MATCHED"))
        row = self.mock.cdrs[f"mm-{TERMINAL}-10-2026-10-13T18:43:00Z"]
        self.assertEqual((row["source"], row["switch_id"], row["ani"], row["charge_cents"]),
                         ("mm_manager", TERMINAL, TERMINAL, 175))

    def test_unanswered_call_releases_hold(self):
        a = self.auth(2, "3125551212")
        c = self.cdr(11, a["auth_code"], "3125551212", 22, call_type=0x51)   # rang 22 s, no answer
        self.assertEqual(c["state"], "FILTERED")
        self.assertEqual(self.mock.holds[a["auth_code"]]["state"], "RELEASED")

    def test_retries(self):
        a1 = self.auth(3, "3125551212")
        a2 = self.auth(3, "3125551212")
        # mm_manager restarted and lost its requested_at: the bridge keeps it.
        a3 = self.auth(3, "3125551212", requested_at="2026-10-13T18:44:10Z")
        self.assertEqual(a1["auth_code"], a2["auth_code"])
        self.assertEqual(a1["auth_code"], a3["auth_code"])
        self.assertEqual(len(self.mock.holds), 1)
        self.assertEqual(self.cdr(12, a1["auth_code"], "3125551212", 95)["state"], "MATCHED")
        d = self.cdr(12, a1["auth_code"], "3125551212", 95)
        self.assertEqual((d["delivered"], d["state"], d["recorded_state"]), (True, "DUPLICATE", "MATCHED"))
        # an unanswered call's release, resent
        b = self.auth(4, "3125551212")
        self.cdr(13, b["auth_code"], "3125551212", 0, call_type=0x51)
        d = self.cdr(13, b["auth_code"], "3125551212", 0, call_type=0x51)
        self.assertEqual((d["state"], d["recorded_state"]), ("DUPLICATE", "FILTERED"))

    def test_caut_transport_retry_sends_identical_body(self):
        self.mock.fail_next_caut = 1
        a = self.auth(5, "3125551212")
        self.assertTrue(a["approved"])
        self.assertEqual(len(self.mock.caut_bodies), 1)      # first attempt never reached CAUT logic

    def test_seven_digit_number(self):
        a = self.auth(6, "5551212")
        self.assertEqual(self.mock.caut_bodies[-1]["called_number"], "2125551212")
        c = self.cdr(14, a["auth_code"], "5551212", 61)
        self.assertEqual(c["state"], "MATCHED")
        row = self.mock.cdrs[f"mm-{TERMINAL}-14-2026-10-13T18:43:00Z"]
        self.assertEqual((row["called_number"], row["call_type"]), ("2125551212", "DOM"))

    def test_international(self):
        a = self.auth(7, "011442071234567", call_type=0x56)
        self.assertEqual(self.mock.caut_bodies[-1]["international"], "yes")
        self.assertEqual(self.mock.holds[a["auth_code"]]["call_type"], "INTL")

    def test_approved_limited_is_invalid_and_released(self):
        a = self.auth(8, "9005551212")
        self.assertFalse(a["approved"])
        self.assertEqual(a["disposition"], "approved_limited")
        (ac, hold), = self.mock.holds.items()
        self.assertEqual(hold["state"], "RELEASED")
        self.assertEqual(self.mock.cdrs[f"mm-{TERMINAL}-A8-2026-10-13T18:42:55Z"]["state"], "FILTERED")

    def test_cardman_down_declines(self):
        self.cm.shutdown()
        self.cm.server_close()
        self.assertFalse(self.auth(9, "3125551212")["approved"])
        c = self.cdr(15, "A1B2C3D4", "3125551212", 95)
        self.assertEqual((c["delivered"], c["retryable"]), (False, True))

    def test_bad_input_declines(self):
        r = self.post("/authorize", {"pan": "12", "pin": TEST_PIN, "terminal_id": TERMINAL,
                                     "seq_num": 1, "requested_at": "2026-10-13T18:42:55Z"})
        self.assertFalse(r["approved"])
        self.assertEqual(self.mock.caut_bodies, [])

    @unittest.skipUnless(os.environ.get("MM_CARDMAN_DRIVE"), "set MM_CARDMAN_DRIVE to the built driver")
    def test_fork_glue_through_bridge(self):
        drive = os.environ["MM_CARDMAN_DRIVE"]
        env = dict(os.environ, CARDMAN_BRIDGE_ADDR=self.addr, TZ="UTC")

        def run(*args):
            out = subprocess.run([drive, *args], env=env, capture_output=True, text=True, timeout=60)
            self.assertNotIn(TEST_PAN, out.stdout + out.stderr)
            self.assertNotIn(TEST_PIN, out.stdout + out.stderr)
            # the driver's own result lines; the glue also prints log lines
            return [w for w in out.stdout.split() if w.startswith(("resp_code=", "auth_code=", "cdr="))]

        # answered call, auth retried by the terminal (same process)
        r = run("auth", TERMINAL, TEST_PAN, TEST_PIN, "3125551212", "21", "0x55", "2")
        self.assertEqual(r[0], "resp_code=0")
        self.assertEqual(r[1], r[3])                     # same auth_code on the retry
        ac = r[1].split("=")[1]
        self.assertEqual(len(self.mock.holds), 1)
        self.assertEqual(run("cdr", TERMINAL, TEST_PAN, "3125551212", "40", ac, "20261013184300",
                             "95", "0x55", "175"), ["cdr=1"])
        self.assertEqual(run("cdr", TERMINAL, TEST_PAN, "3125551212", "40", ac, "20261013184300",
                             "95", "0x55", "175"), ["cdr=1"])        # resend -> DUPLICATE, delivered
        cid = f"mm-{TERMINAL}-40-2026-10-13T18:43:00Z"
        self.assertEqual(self.mock.cdrs[cid]["state"], "MATCHED")
        # unanswered call with ring time; 7-digit dialing
        r = run("auth", TERMINAL, TEST_PAN, TEST_PIN, "5551212", "22", "0x53")
        ac2 = r[1].split("=")[1]
        self.assertEqual(self.mock.caut_bodies[-1]["called_number"], "2125551212")
        self.assertEqual(run("cdr", TERMINAL, TEST_PAN, "5551212", "41", ac2, "20261013184500",
                             "20", "0x51", "0"), ["cdr=1"])
        self.assertEqual(self.mock.holds[ac2]["state"], "RELEASED")
        # declined: wrong PIN -> invalid, no auth_code
        self.assertEqual(run("auth", TERMINAL, TEST_PAN, "1357", "3125551212", "23", "0x55"),
                         ["resp_code=1", "auth_code=-"])
        # a CDR without a CARDMAN auth_code is not forwarded
        self.assertEqual(run("cdr", TERMINAL, TEST_PAN, "3125551212", "42", "-", "20261013184600",
                             "30", "0x55", "0"), ["cdr=0"])


@unittest.skipUnless(os.environ.get("MM_CARDMAN_DRIVE"), "set MM_CARDMAN_DRIVE to the built driver")
class ForkRejectsBrokenBridge(unittest.TestCase):
    """The C client must fail closed on a bridge response that is not a
    complete HTTP 200 carrying one flat JSON object, even when the bytes
    it did get say approved/delivered."""

    APPROVED = b'{"approved": true, "disposition": "approved", "auth_code": "0Q7KX2LM"}'
    DELIVERED = b'{"delivered": true, "state": "MATCHED"}'

    def raw_bridge(self, reply):
        import socket as so
        srv = so.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(4)

        def serve_one():
            while True:
                try:
                    c, _ = srv.accept()
                except OSError:
                    return
                with c:
                    c.settimeout(5)
                    c.recv(4096)
                    c.sendall(reply)
        threading.Thread(target=serve_one, daemon=True).start()
        self.addCleanup(srv.close)
        return f"127.0.0.1:{srv.getsockname()[1]}"

    def run_drive(self, addr, *args):
        out = subprocess.run([os.environ["MM_CARDMAN_DRIVE"], *args], capture_output=True, text=True, timeout=60,
                             env=dict(os.environ, CARDMAN_BRIDGE_ADDR=addr, TZ="UTC"))
        return [w for w in out.stdout.split() if w.startswith(("resp_code=", "auth_code=", "cdr="))]

    def auth(self, reply):
        return self.run_drive(self.raw_bridge(reply), "auth", TERMINAL, TEST_PAN, TEST_PIN, "3125551212", "31", "0x55")

    def cdr(self, reply):
        return self.run_drive(self.raw_bridge(reply), "cdr", TERMINAL, TEST_PAN, "3125551212", "31", "0Q7KX2LM",
                              "20261013184300", "95", "0x55", "175")

    def frame(self, body, length=None, status=b"200 OK"):
        n = len(body) if length is None else length
        return b"HTTP/1.0 " + status + b"\r\nContent-Length: " + str(n).encode() + b"\r\n\r\n" + body

    def test_well_formed_still_works(self):
        self.assertEqual(self.auth(self.frame(self.APPROVED)), ["resp_code=0", "auth_code=0Q7KX2LM"])
        self.assertEqual(self.cdr(self.frame(self.DELIVERED)), ["cdr=1"])

    def test_truncated_body_fails_closed(self):
        self.assertEqual(self.auth(self.frame(self.APPROVED[:-10], length=len(self.APPROVED))),
                         ["resp_code=1", "auth_code=-"])
        self.assertEqual(self.cdr(self.frame(self.DELIVERED[:-5], length=len(self.DELIVERED))), ["cdr=-1"])

    def test_malformed_json_fails_closed(self):
        for body in (b'{"approved": true, "auth_code": "0Q7KX2LM"} trailing',
                     b'{"approved": false, "approved": true, "auth_code": "0Q7KX2LM"}',
                     b'{"x": {"approved": true, "auth_code": "0Q7KX2LM"}}',
                     b'{"approved": true, "auth_code": "0Q7KX2LM",}'):
            self.assertEqual(self.auth(self.frame(body)), ["resp_code=1", "auth_code=-"], body)
        self.assertEqual(self.cdr(self.frame(b'{"note": "x", "delivered": true')), ["cdr=-1"])

    def test_no_content_length_or_non_200_fails_closed(self):
        self.assertEqual(self.auth(b"HTTP/1.0 200 OK\r\n\r\n" + self.APPROVED), ["resp_code=1", "auth_code=-"])
        self.assertEqual(self.auth(self.frame(self.APPROVED, status=b"500 Oops")), ["resp_code=1", "auth_code=-"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

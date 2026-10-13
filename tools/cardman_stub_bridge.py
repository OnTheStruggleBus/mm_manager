#!/usr/bin/env python3
"""
Minimal stub CARDMAN WAPI bridge for local Phase 1 testing of
mm_manager's cardman_bridge_authorize() client (src/mm_cardman_bridge.c).

Not a real bridge - no TLS, no real CAUT call, no CARDMAN identity. Just
enough of an HTTP/JSON contract to prove mm_manager's patched auth
handler calls out correctly and fails closed when it should. Real bridge
implementation is Phase 2 (see mm-cardman-integration repo: SOW.md).

Usage:
    python3 tools/cardman_stub_bridge.py [--port 8420] [--decline]

Approves every request by default (returns a fixed auth_code). Pass
--decline to test the fail-closed path instead.
"""
import argparse
import http.server
import json


class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        try:
            req = json.loads(body)
        except json.JSONDecodeError:
            req = {}

        print(f"stub bridge: {self.path} terminal_id={req.get('terminal_id')!r}, "
              f"pan=***masked-in-log***, called_number={req.get('called_number')!r}, "
              f"seq_num={req.get('seq_num')!r}, requested_at={req.get('requested_at')!r}, "
              f"international={req.get('international')!r}")

        if self.path == "/cdr":
            resp = {"delivered": not self.server.decline, "state": "MATCHED"}
        elif self.server.decline:
            resp = {"approved": False, "disposition": "declined"}
        else:
            # 8 chars of [0-9A-Z], the shape CARDMAN's DERIVE_AC produces.
            resp = {"approved": True, "disposition": "approved", "auth_code": "A1B2C3D4"}

        payload = json.dumps(resp).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        pass  # quiet; the do_POST print above is enough for this stub


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8420)
    parser.add_argument("--decline", action="store_true",
                         help="Decline every request, to test mm_manager's fail-closed path.")
    args = parser.parse_args()

    server = http.server.HTTPServer(("127.0.0.1", args.port), Handler)
    server.decline = args.decline
    print(f"cardman stub bridge listening on 127.0.0.1:{args.port} "
          f"({'declining' if args.decline else 'approving'} every request)")
    server.serve_forever()


if __name__ == "__main__":
    main()

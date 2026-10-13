#!/usr/bin/env bash
# cardman_live_test.sh -- drive the fork's glue and tools/cardman_bridge.py
# against a live CARDMAN 2.5.0.1 dev container (MCardSys v2.5.0.1).
#
# Prerequisites (on the machine running Docker):
#   1. In an MCardSys checkout:
#        DOCKER_DEFAULT_PLATFORM=linux/amd64 docker compose build
#        docker compose run --rm --service-ports cardman shell
#      and inside that shell, ./scripts/seed.sh (rates + TELCO01).
#   2. In this repo: cmake -B build && cmake --build build
#   3. Run from this repo's root:  tools/cardman_live_test.sh
#
# Env: CONTAINER (default: the running cardman-test:local container),
#      CARDMAN_URL (default https://127.0.0.1:8443), PP_USER / PP_PASS
#      (default the dev identity mm-dev / mm-dev_dev), TERMINAL.
#
# Uses a freshly issued test card. The card number and PIN are never
# printed; only masked forms appear in output.
set -uo pipefail
cd "$(dirname "$0")/.."

CONTAINER="${CONTAINER:-$(docker ps -q --filter ancestor=cardman-test:local | head -1)}"
CARDMAN_URL="${CARDMAN_URL:-https://127.0.0.1:8443}"
PP_USER="${PP_USER:-mm-dev}"
PP_PASS="${PP_PASS:-mm-dev_dev}"
TERMINAL="${TERMINAL:-2125551000}"
DRIVE=./build/mm_cardman_drive
WORK=$(mktemp -d)
BRIDGE_PORT=$((18400 + RANDOM % 500))
FAILED=0
trap 'kill $BRIDGE_PID 2>/dev/null; rm -rf "$WORK"' EXIT

pass() { echo "   OK: $1"; }
fail() { echo "   FAIL: $1"; FAILED=1; }
[ -n "$CONTAINER" ] || { echo "FAIL: no running cardman-test:local container"; exit 1; }
[ -x "$DRIVE" ] || { echo "FAIL: build $DRIVE first"; exit 1; }

# Runs SQL inside the container, with the DB settings verify-payphone.sh uses.
sql() {
  docker exec -i "$CONTAINER" bash -c 'cd /app; cnf() { sed -nE "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*(.*[^[:space:]])[[:space:]]*$/\1/p" bricks/conf/bricks.cnf | head -1; };
    PGPASSWORD="$(cnf db_password)" psql -h "$(cnf db_host)" -p "$(cnf db_port)" -U "$(cnf db_user)" -d millennium -tAq' <<<"$1"
}

echo "== setup: payphone cert, a fresh test card, the bridge =="
for f in ca.pem client-payphone.pem client-payphone-key.pem; do
  docker cp -q "$CONTAINER:/app/bricks/conf/$f" "$WORK/$f" || { echo "FAIL: docker cp $f"; exit 1; }
done
NEWC=$(docker exec "$CONTAINER" bash -c 'cd /app && curl -sS -m 15 --cert bricks/conf/client-issuer.pem --key bricks/conf/client-issuer-key.pem --cacert bricks/conf/ca.pem -u "issuer:${ISSUER_PASS:-issuer_dev}" -H "Content-Type: application/json" -X POST https://127.0.0.1:8443/api/newcust -d "{\"initial_balance_cents\":2000,\"telco_id\":\"TELCO01\"}"')
PAN=$(sed -nE 's/.*"pan"[[:space:]]*:[[:space:]]*"([0-9]{16})".*/\1/p' <<<"$NEWC")
PIN=$(sed -nE 's/.*"pin"[[:space:]]*:[[:space:]]*"([0-9]{4})".*/\1/p' <<<"$NEWC")
[ -n "$PAN" ] && [ -n "$PIN" ] || { echo "FAIL: NEWC did not issue a card (run ./scripts/seed.sh in the container)"; exit 1; }
echo "   card ${PAN:0:6}****${PAN:12:4}"

CARDMAN_BRIDGE_PASSWORD="$PP_PASS" python3 tools/cardman_bridge.py --listen "127.0.0.1:$BRIDGE_PORT" \
  --cardman-url "$CARDMAN_URL" --client-cert "$WORK/client-payphone.pem" \
  --client-key "$WORK/client-payphone-key.pem" --ca-cert "$WORK/ca.pem" --user "$PP_USER" \
  --telco-id TELCO01 2>"$WORK/bridge.log" &
BRIDGE_PID=$!
sleep 1
export CARDMAN_BRIDGE_ADDR="127.0.0.1:$BRIDGE_PORT"
export TZ=UTC     # the driver's CDR start times are terminal-local; keep them UTC here

# drive ARGS... -> result tokens only; fails the run if the card leaks
drive() {
  local out; out=$("$DRIVE" "$@" 2>&1)
  case "$out" in *"$PAN"*|*"$PIN"*) fail "card data in driver output";; esac
  grep -oE '(resp_code|auth_code|cdr)=[^ ]+' <<<"$out" | tr '\n' ' '
}
code_of() { grep -oE 'auth_code=[0-9A-Z]{8}' <<<"$1" | head -1 | cut -d= -f2; }

S=$((1000 + RANDOM % 60000))
NOW=$(date -u +%Y%m%d%H%M%S)

echo "== 1. approved, answered call =="
R=$(drive auth "$TERMINAL" "$PAN" "$PIN" 3125551212 "$S" 0x55)
AC1=$(code_of "$R")
[[ "$R" == resp_code=0* ]] && [ -n "$AC1" ] && pass "valid, auth_code=$AC1" || fail "auth: $R"
R=$(drive cdr "$TERMINAL" "$PAN" 3125551212 "$S" "$AC1" "$NOW" 95 0x55 175)
[ "$R" = "cdr=1 " ] && pass "CDR delivered" || fail "cdr: $R"
[ "$(sql "SELECT state FROM holds WHERE auth_code='$AC1'")" = CAPTURED ] && pass "hold CAPTURED" || fail "hold not CAPTURED"

echo "== 2. approved, unanswered call (22 s of ringing) =="
R=$(drive auth "$TERMINAL" "$PAN" "$PIN" 3125551212 $((S + 1)) 0x55)
AC2=$(code_of "$R")
R=$(drive cdr "$TERMINAL" "$PAN" 3125551212 $((S + 1)) "$AC2" "$NOW" 22 0x51 0)
[ "$R" = "cdr=1 " ] && pass "release CDR delivered" || fail "cdr: $R"
[ "$(sql "SELECT state FROM holds WHERE auth_code='$AC2'")" = RELEASED ] && pass "hold RELEASED" || fail "hold not RELEASED"

echo "== 3. retries: same auth_code, one hold, DUPLICATE CDRs =="
R=$(drive auth "$TERMINAL" "$PAN" "$PIN" 3125551212 "$S" 0x55 2)        # new process: bridge cache
[ "$(grep -oE 'auth_code=[0-9A-Z]{8}' <<<"$R" | sort -u)" = "auth_code=$AC1" ] \
  && pass "auth retries return $AC1" || fail "retry changed auth_code: $R"
[ "$(sql "SELECT count(*) FROM holds WHERE pan='$PAN'")" = 2 ] && pass "still 2 holds for the card" || fail "extra hold placed"
drive cdr "$TERMINAL" "$PAN" 3125551212 "$S" "$AC1" "$NOW" 95 0x55 175 >/dev/null
drive cdr "$TERMINAL" "$PAN" 3125551212 $((S + 1)) "$AC2" "$NOW" 22 0x51 0 >/dev/null
[ "$(grep -c 'state=DUPLICATE' "$WORK/bridge.log")" -ge 2 ] && pass "both resends DUPLICATE" || fail "resends not DUPLICATE"

echo "== 4. 7-digit local number =="
R=$(drive auth "$TERMINAL" "$PAN" "$PIN" 5551212 $((S + 2)) 0x53)
AC4=$(code_of "$R")
drive cdr "$TERMINAL" "$PAN" 5551212 $((S + 2)) "$AC4" "$NOW" 61 0x53 0 >/dev/null
CN=$(sql "SELECT called_number FROM cdr WHERE auth_code='$AC4'")
[ "$CN" = "${TERMINAL:0:3}5551212" ] && pass "rated as $CN" || fail "called_number = '$CN'"

echo "== 5. approved_limited (high-risk NPA 900) is invalid and released =="
R=$(drive auth "$TERMINAL" "$PAN" "$PIN" 9005551212 $((S + 3)) 0x55)
[[ "$R" == resp_code=1* ]] && pass "invalid at the terminal" || fail "auth: $R"
AC5=$(grep -oE 'disposition=approved_limited auth_code=[0-9A-Z]{8}' "$WORK/bridge.log" | tail -1 | cut -d= -f3)
ST=$(sql "SELECT state FROM holds WHERE auth_code='$AC5'")
[ "$ST" = RELEASED ] && pass "its hold RELEASED" || fail "approved_limited hold state '$ST'"

echo "== 6. actor stamps and cdr rows =="
A=$(sql "SELECT actor FROM journal WHERE auth_code='$AC1' AND type='HOLD'")
[ "$A" = "platform:$PP_USER" ] && pass "HOLD actor $A" || fail "HOLD actor '$A'"
A=$(sql "SELECT actor FROM journal WHERE auth_code='$AC1' AND type='CAPTURE'")
[ "$A" = "mediation:$PP_USER" ] && pass "CAPTURE actor $A" || fail "CAPTURE actor '$A'"
ROW=$(sql "SELECT source||'|'||switch_id||'|'||ani||'|'||reported_charge_cents FROM cdr WHERE auth_code='$AC1' ORDER BY received_ts LIMIT 1")
[ "$ROW" = "mm_manager|$TERMINAL|$TERMINAL|175" ] && pass "cdr source|switch_id|ani|reported = $ROW" || fail "cdr row '$ROW'"

if grep -q -e "$PAN" -e "\"$PIN\"" "$WORK/bridge.log"; then fail "card data in bridge log"; else pass "bridge log has no card data"; fi

echo
echo "bridge log: (kept only for this run)"; sed 's/^/   /' "$WORK/bridge.log"
[ "$FAILED" -eq 0 ] && { echo "RESULT: PASS"; exit 0; }
echo "RESULT: FAILURES ABOVE"; exit 1

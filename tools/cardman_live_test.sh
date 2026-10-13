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
#      (needs bash, python3, sqlite3 and the docker CLI)
#
# Without a local toolchain, steps 2 and 3 can run in any Linux container
# with cmake, gcc, g++, python3, sqlite3 and the docker CLI, sharing the
# CARDMAN container's network (so 127.0.0.1:8443 is CARDMAN):
#   docker run --rm --network container:<id> -e CONTAINER=<id> \
#     -v /var/run/docker.sock:/var/run/docker.sock -v "$PWD":/src -w /src <image> \
#     bash -c 'cmake -B build && cmake --build build && tools/cardman_live_test.sh'
#
# Env: CONTAINER (default: the running cardman-test:local container),
#      CARDMAN_URL (default https://127.0.0.1:8443), PP_USER (default the
#      dev identity mm-dev), TERMINAL.
# Required: PP_PASS (the payphone identity's password) and ISSUER_PASS
#      (the issuer's, to mint a test card). Never stored in this repo
#      (AI/SECURITY_RULES.md rule 8); for a dev container they are the
#      defaults in MCardSys scripts/verify-payphone.sh.
#
# Uses a freshly issued test card. The card number and PIN are never
# printed; only masked forms appear in output. The whole run's output,
# the bridge log and the driver's mm_manager.db (TAUTH/TCDR rows written
# by the production accounting code) are checked for them at the end.
set -uo pipefail
cd "$(dirname "$0")/.."

CONTAINER="${CONTAINER:-$(docker ps -q --filter ancestor=cardman-test:local | head -1)}"
CARDMAN_URL="${CARDMAN_URL:-https://127.0.0.1:8443}"
PP_USER="${PP_USER:-mm-dev}"
PP_PASS="${PP_PASS:-}"
ISSUER_PASS="${ISSUER_PASS:-}"
TERMINAL="${TERMINAL:-2125551000}"
DRIVE=./build/mm_cardman_drive
WORK=$(mktemp -d)
BRIDGE_PORT=$((18400 + RANDOM % 500))
FAILED=0
BRIDGE_PID=
trap 'kill $BRIDGE_PID 2>/dev/null; rm -rf "$WORK"' EXIT

# Everything printed from here on also goes to $WORK/run.log, which the
# leak check reads. A FIFO, not >(...), so it works in macOS's bash 3.2.
mkfifo "$WORK/out"
tee "$WORK/run.log" <"$WORK/out" &
TEE_PID=$!
exec 3>&1 4>&2 >"$WORK/out" 2>&1

pass() { echo "   OK: $1"; }
fail() { echo "   FAIL: $1"; FAILED=1; }
[ -n "$PP_PASS" ] && [ -n "$ISSUER_PASS" ] || { echo "FAIL: set PP_PASS and ISSUER_PASS (see the header)"; exit 1; }
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
NEWC=$(ISSUER_PASS="$ISSUER_PASS" docker exec -e ISSUER_PASS "$CONTAINER" bash -c 'cd /app && curl -sS -m 15 --cert bricks/conf/client-issuer.pem --key bricks/conf/client-issuer-key.pem --cacert bricks/conf/ca.pem -u "issuer:$ISSUER_PASS" -H "Content-Type: application/json" -X POST https://127.0.0.1:8443/api/newcust -d "{\"initial_balance_cents\":2000,\"telco_id\":\"TELCO01\"}"')
PAN=$(sed -nE 's/.*"pan"[[:space:]]*:[[:space:]]*"([0-9]{16})".*/\1/p' <<<"$NEWC")
PIN=$(sed -nE 's/.*"pin"[[:space:]]*:[[:space:]]*"([0-9]{4})".*/\1/p' <<<"$NEWC")
[ -n "$PAN" ] && [ -n "$PIN" ] || { echo "FAIL: NEWC did not issue a card (run ./scripts/seed.sh in the container)"; exit 1; }
echo "   card ${PAN:0:6}****${PAN:12:4}"

CARDMAN_BRIDGE_PASSWORD="$PP_PASS" python3 tools/cardman_bridge.py --listen "127.0.0.1:$BRIDGE_PORT" \
  --cardman-url "$CARDMAN_URL" --client-cert "$WORK/client-payphone.pem" \
  --client-key "$WORK/client-payphone-key.pem" --ca-cert "$WORK/ca.pem" --user "$PP_USER" \
  --telco-id TELCO01 >/dev/null 2>"$WORK/bridge.log" &
BRIDGE_PID=$!
sleep 1
export CARDMAN_BRIDGE_ADDR="127.0.0.1:$BRIDGE_PORT"
# The driver's CDR start time is terminal-local. Run in a zone that is
# not UTC so the fork's local -> UTC conversion (start_ts, cdr_id) is
# exercised; a POSIX TZ string needs no tzdata.
export TZ='EST5EDT,M3.2.0,M11.1.0'
export MM_CARDMAN_DRIVE_DB="$WORK/mm_manager.db"

# has_pin FILE: the PIN as a whole token. A bare substring would match
# inside phone numbers and timestamps.
has_pin() { grep -qE "(^|[^0-9])$PIN([^0-9]|\$)" "$1"; }

# drive ARGS... -> result tokens only; fails the run if the card leaks
drive() {
  local out; out=$("$DRIVE" "$@" 2>&1)
  printf '%s\n' "$out" > "$WORK/drive.out"
  if grep -qF "$PAN" "$WORK/drive.out" || has_pin "$WORK/drive.out"; then fail "card data in driver output"; fi
  grep -oE '(resp_code|auth_code|cdr)=[^ ]+' <<<"$out" | tr '\n' ' '
}
code_of() { grep -oE 'auth_code=[0-9A-Z]{8}' <<<"$1" | head -1 | cut -d= -f2; }

S=$((1000 + RANDOM % 60000))
EPOCH=$(date +%s)
# fmt +FORMAT [-u]: $EPOCH formatted; GNU date, else BSD (options first).
fmt() { date ${2:-} -d "@$EPOCH" "$1" 2>/dev/null || date ${2:-} -r "$EPOCH" "$1"; }
NOW=$(fmt +%Y%m%d%H%M%S)                    # terminal-local, what the CDR carries
NOW_UTC=$(fmt +%Y-%m-%dT%H:%M:%SZ -u)       # what CARDMAN must receive

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

echo "== 4b. international call (call type 0x56) gets an INTL hold =="
# seed.sh's TELCO01 tariff, 180 s estimate: INTL 50+40+35 = 125c, DOM 300c.
INTL_NO=011441632960000                    # UK drama range, as dialed
R=$(drive auth "$TERMINAL" "$PAN" "$PIN" "$INTL_NO" $((S + 5)) 0x56)
AC4B=$(code_of "$R")
[[ "$R" == resp_code=0* ]] && [ -n "$AC4B" ] && pass "valid, auth_code=$AC4B" || fail "auth: $R"
H=$(sql "SELECT amount_cents||'|'||called_number FROM holds WHERE auth_code='$AC4B'")
[ "$H" = "125|$INTL_NO" ] && pass "INTL hold amount|number = $H" || fail "hold '$H', want 125|$INTL_NO"
drive cdr "$TERMINAL" "$PAN" "$INTL_NO" $((S + 5)) "$AC4B" "$NOW" 61 0x56 0 >/dev/null
ROW=$(sql "SELECT state||'|'||called_number FROM cdr WHERE auth_code='$AC4B'")
[ "$ROW" = "MATCHED|$INTL_NO" ] && [ "$(sql "SELECT state FROM holds WHERE auth_code='$AC4B'")" = CAPTURED ] \
  && pass "CDR MATCHED as dialed, hold CAPTURED" || fail "cdr '$ROW'"

echo "== 5. approved_limited (high-risk NPA 900) is invalid and released =="
R=$(drive auth "$TERMINAL" "$PAN" "$PIN" 9005551212 $((S + 3)) 0x55)
[[ "$R" == resp_code=1* ]] && pass "invalid at the terminal" || fail "auth: $R"
AC5=$(grep -oE 'disposition=approved_limited auth_code=[0-9A-Z]{8}' "$WORK/bridge.log" | tail -1 | cut -d= -f3)
ST=$(sql "SELECT state FROM holds WHERE auth_code='$AC5'")
[ "$ST" = RELEASED ] && pass "its hold RELEASED" || fail "approved_limited hold state '$ST'"

echo "== 5b. a CARDMAN decline (wrong PIN) is invalid, no hold =="
BAD_PIN=$(printf %04d $(( (10#$PIN + 1) % 10000 )))
HOLDS=$(sql "SELECT count(*) FROM holds WHERE pan='$PAN'")
R=$(drive auth "$TERMINAL" "$PAN" "$BAD_PIN" 3125551212 $((S + 4)) 0x55)
[[ "$R" == "resp_code=1 auth_code=- " ]] && pass "invalid at the terminal, no auth_code" || fail "auth: $R"
grep -q "seq=$((S + 4)) .*disposition=declined auth_code=None -> invalid" "$WORK/bridge.log" \
  && pass "bridge saw disposition=declined" || fail "no declined line in bridge log"
[ "$(sql "SELECT count(*) FROM holds WHERE pan='$PAN'")" = "$HOLDS" ] && pass "no hold placed" || fail "decline placed a hold"

echo "== 6. actor stamps and cdr rows =="
A=$(sql "SELECT actor FROM journal WHERE auth_code='$AC1' AND type='HOLD'")
[ "$A" = "platform:$PP_USER" ] && pass "HOLD actor $A" || fail "HOLD actor '$A'"
A=$(sql "SELECT actor FROM journal WHERE auth_code='$AC1' AND type='CAPTURE'")
[ "$A" = "mediation:$PP_USER" ] && pass "CAPTURE actor $A" || fail "CAPTURE actor '$A'"
ROW=$(sql "SELECT source||'|'||switch_id||'|'||ani||'|'||reported_charge_cents FROM cdr WHERE auth_code='$AC1' ORDER BY received_ts LIMIT 1")
[ "$ROW" = "mm_manager|$TERMINAL|$TERMINAL|175" ] && pass "cdr source|switch_id|ani|reported = $ROW" || fail "cdr row '$ROW'"
ROW=$(sql "SELECT cdr_id||'|'||to_char(start_ts AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') FROM cdr WHERE auth_code='$AC1' ORDER BY received_ts LIMIT 1")
[ "$ROW" = "mm-$TERMINAL-$S-$NOW_UTC|$NOW_UTC" ] && pass "terminal-local $NOW ($TZ) arrived as start_ts $NOW_UTC" \
  || fail "cdr_id|start_ts '$ROW', want UTC $NOW_UTC"

echo "== 7. card data at rest and in output =="
DB="$MM_CARDMAN_DRIVE_DB"
MASKED="${PAN:0:6}****${PAN:12:4}"
lite() { sqlite3 "$DB" "$1"; }
# One row per (terminal, seq): TAUTH and TCDR are UNIQUE on it and the
# retries/resends above are deduplicated, so 6 auths and 4 CDRs.
N=$(lite "SELECT count(*) FROM TAUTH WHERE CARD_NUMBER='$MASKED'")
[ "$N" = 6 ] && [ "$(lite "SELECT count(*) FROM TAUTH WHERE CARD_NUMBER<>'$MASKED'")" = 0 ] \
  && pass "TAUTH: $N rows, CARD_NUMBER only ever $MASKED" || fail "TAUTH CARD_NUMBER not all $MASKED"
N=$(lite "SELECT count(*) FROM TCDR WHERE CARD='$MASKED'")
[ "$N" = 4 ] && [ "$(lite "SELECT count(*) FROM TCDR WHERE CARD<>'$MASKED'")" = 0 ] \
  && pass "TCDR: $N rows, CARD only ever $MASKED" || fail "TCDR CARD not all $MASKED"
[ -z "$(lite "SELECT name FROM pragma_table_info('TAUTH') WHERE name LIKE '%PIN%'
              UNION SELECT name FROM pragma_table_info('TCDR') WHERE name LIKE '%PIN%'")" ] \
  && pass "TAUTH/TCDR have no PIN column" || fail "PIN column in TAUTH/TCDR"
# Rule 9: also look for packed BCD (a blob's hex is the digits themselves)
# and for ASCII digits stored as a blob (hex 3x per digit).
sqlite3 "$DB" .dump > "$WORK/db.dump"
PAN_HEX=$(printf '%s' "$PAN" | od -An -tx1 | tr -d ' \n')
if grep -qiF -e "$PAN" -e "$PAN_HEX" "$WORK/db.dump" || grep -qE "'$PIN'" "$WORK/db.dump"; then
  fail "card data in mm_manager.db"; else pass "mm_manager.db dump has no PAN (text, BCD, ASCII blob) or PIN"; fi
if grep -qF "$PAN" "$WORK/bridge.log" || has_pin "$WORK/bridge.log"; then fail "card data in bridge log"; else pass "bridge log has no card data"; fi
kill $BRIDGE_PID 2>/dev/null; BRIDGE_PID=

echo
echo "bridge log: (kept only for this run)"; sed 's/^/   /' "$WORK/bridge.log"

# Close the FIFO, let tee finish, then check everything this run printed.
exec 1>&3 2>&4 3>&- 4>&-
wait "$TEE_PID"
if grep -qF "$PAN" "$WORK/run.log" || has_pin "$WORK/run.log"; then
  echo "   FAIL: card data in this run's output"; FAILED=1
else
  echo "   OK: this run's output ($(wc -l <"$WORK/run.log" | tr -d ' ') lines) has no PAN or PIN"
fi
[ "$FAILED" -eq 0 ] && { echo "RESULT: PASS"; exit 0; }
echo "RESULT: FAILURES ABOVE"; exit 1

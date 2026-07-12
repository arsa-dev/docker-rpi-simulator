#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# run_web.sh — integration tests for the web panel + control API (Milestone 4).
#
# Proves the web layer shares the one model: HTTP drives, the FUSE files reflect it,
# the WebSocket handshake is byte-correct and pushes state, and — the headline — an
# HTTP panel toggle of an input wakes a process blocked in poll() on the value file.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
MOUNT=/gpio
PORT=8080
BASE="http://127.0.0.1:$PORT"

pass=0; fail=0
ok()  { echo "  ok   - $1"; pass=$((pass+1)); }
bad() { echo "  FAIL - $1"; fail=$((fail+1)); }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || bad "$1 (expected '$2', got '$3')"; }

cleanup() { [ -n "${DP:-}" ] && kill "$DP" 2>/dev/null; fusermount3 -u "$MOUNT" 2>/dev/null; }
trap cleanup EXIT

echo "== starting daemon (sysfs + control + web) =="
GPIO_WEB_ROOT="$ROOT/web" GPIO_HTTP_PORT=$PORT GPIO_BOARD=pi4 "$BUILD/gpio-sim" "$MOUNT" &
DP=$!
for _ in $(seq 1 50); do
    curl -sf "$BASE/api/state" >/dev/null 2>&1 && break; sleep 0.1
done

echo "== panel + API served =="
code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/")
assert_eq "GET / serves panel" "200" "$code"
state=$(curl -s "$BASE/api/state")
case "$state" in *'"board":"pi4"'*) ok "GET /api/state reports board";; *) bad "state missing board";; esac

echo "== WebSocket handshake (byte-exact) + live state push =="
if "$BUILD/ws_probe" 127.0.0.1 $PORT; then ok "ws handshake + initial frame"; else bad "ws probe failed"; fi

echo "== HTTP drive of an input is visible via FUSE =="
echo 17 > "$MOUNT/export"; echo in > "$MOUNT/gpio17/direction"
curl -s -o /dev/null -X POST -H 'Content-Type: application/json' -d '{"level":1}' "$BASE/api/pin/17"
assert_eq "POST /api/pin/17 -> FUSE value" "1" "$(cat $MOUNT/gpio17/value)"

echo "== driving an OUTPUT via the panel is refused (409) =="
echo 22 > "$MOUNT/export"; echo out > "$MOUNT/gpio22/direction"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d '{"level":1}' "$BASE/api/pin/22")
assert_eq "POST output pin -> 409" "409" "$code"

echo "== HEADLINE: an HTTP panel toggle wakes a blocked poll() =="
curl -s -o /dev/null -X POST "$BASE/api/reboot"
echo 20 > "$MOUNT/export"; echo in > "$MOUNT/gpio20/direction"; echo rising > "$MOUNT/gpio20/edge"
RDY=/tmp/rdy.web; rm -f "$RDY"
"$BUILD/poll_client" "$MOUNT" 20 rising 3000 "$RDY" & PC=$!
for _ in $(seq 1 30); do [ -e "$RDY" ] && break; sleep 0.1; done
curl -s -o /dev/null -X POST -d '{"level":1}' "$BASE/api/pin/20"   # rising, via HTTP
wait $PC; rc=$?
assert_eq "poll(rising) woke from HTTP toggle" "0" "$rc"

echo "== POST /api/reboot resets =="
echo 7 > "$MOUNT/export"
curl -s -o /dev/null -X POST "$BASE/api/reboot"
assert_eq "reboot cleared gpio7" "no" "$(test -d $MOUNT/gpio7 && echo yes || echo no)"

echo
echo "RESULT(web): $pass passed, $fail failed"
[ "$fail" -eq 0 ]

#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# run.sh — integration tests that exercise the REAL FUSE filesystem on Linux.
#
# Runs inside the dev container (needs /dev/fuse + SYS_ADMIN + apparmor=unconfined).
# Starts the daemon, then asserts the sysfs semantics and — the headline — that a
# process blocked in epoll(value) wakes on the matching edge and NOT on the wrong one.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
MOUNT=/gpio
SOCK=/run/gpio-sim.sock

pass=0; fail=0
ok()   { echo "  ok   - $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL - $1"; fail=$((fail+1)); }
# assert_eq <desc> <expected> <actual>
assert_eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi; }
# assert_ok <desc> <cmd...>  — command must succeed
assert_ok() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi; }
# assert_fail <desc> <cmd...> — command must fail
assert_fail() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then bad "$d (unexpectedly succeeded)"; else ok "$d"; fi; }

cleanup() {
    [ -n "${DAEMON_PID:-}" ] && kill "$DAEMON_PID" 2>/dev/null
    fusermount3 -u "$MOUNT" 2>/dev/null || umount "$MOUNT" 2>/dev/null
}
trap cleanup EXIT

echo "== starting daemon =="
GPIO_LOG_LEVEL=info "$BUILD/gpio-sim" "$MOUNT" &
DAEMON_PID=$!

# Wait for the mount to come up.
for _ in $(seq 1 50); do
    if cat "$MOUNT/gpiochip0/base" >/dev/null 2>&1; then break; fi
    sleep 0.1
done
if ! cat "$MOUNT/gpiochip0/base" >/dev/null 2>&1; then
    echo "daemon failed to mount at $MOUNT"; exit 1
fi

echo "== chip attributes =="
assert_eq "gpiochip0/base"  "0"               "$(cat $MOUNT/gpiochip0/base)"
assert_eq "gpiochip0/ngpio" "28"              "$(cat $MOUNT/gpiochip0/ngpio)"
assert_eq "gpiochip0/label" "pinctrl-bcm2835" "$(cat $MOUNT/gpiochip0/label)"

echo "== export / unexport =="
echo 17 > "$MOUNT/export"
assert_ok   "export 17 creates gpio17/"  test -d "$MOUNT/gpio17"
assert_fail "duplicate export -> EBUSY"  bash -c "echo 17 > $MOUNT/export"
assert_fail "out-of-range export -> EINVAL" bash -c "echo 999 > $MOUNT/export"

echo "== direction (incl. glitch-free high/low) =="
echo out > "$MOUNT/gpio17/direction"
assert_eq "direction reads out" "out" "$(cat $MOUNT/gpio17/direction)"
echo high > "$MOUNT/gpio17/direction"
assert_eq "high => value 1"     "1"   "$(cat $MOUNT/gpio17/value)"
echo low > "$MOUNT/gpio17/direction"
assert_eq "low => value 0"      "0"   "$(cat $MOUNT/gpio17/value)"

echo "== value read/write + active_low =="
echo out > "$MOUNT/gpio17/direction"
echo 1 > "$MOUNT/gpio17/value"
assert_eq "value write 1 reads back" "1" "$(cat $MOUNT/gpio17/value)"
echo 1 > "$MOUNT/gpio17/active_low"
assert_eq "active_low inverts read"  "0" "$(cat $MOUNT/gpio17/value)"
echo 0 > "$MOUNT/gpio17/active_low"
assert_eq "active_low off restores"  "1" "$(cat $MOUNT/gpio17/value)"

echo "== writing value on an input fails (EPERM) =="
echo in > "$MOUNT/gpio17/direction"
assert_fail "echo 1 > value on input" bash -c "echo 1 > $MOUNT/gpio17/value"

echo "== unexport removes the node =="
echo 17 > "$MOUNT/unexport"
assert_ok "unexport removes gpio17/" test ! -d "$MOUNT/gpio17"

echo "== control API <-> FUSE consistency =="
"$BUILD/sim_ctl" "$SOCK" export 5      >/dev/null
"$BUILD/sim_ctl" "$SOCK" direction 5 out >/dev/null
"$BUILD/sim_ctl" "$SOCK" value 5 1     >/dev/null
assert_eq "control write visible via FUSE" "1" "$(cat $MOUNT/gpio5/value)"
echo 6 > "$MOUNT/export"; echo out > "$MOUNT/gpio6/direction"; echo 1 > "$MOUNT/gpio6/value"
assert_eq "FUSE write visible via control" "ok 1" "$("$BUILD/sim_ctl" "$SOCK" read 6 | tr -d '\n')"

echo "== EDGE / POLL: wakes on matching edge =="
"$BUILD/sim_ctl" "$SOCK" reboot >/dev/null
RDY=/tmp/rdy.pos; rm -f "$RDY"
"$BUILD/poll_client" "$MOUNT" 20 rising 3000 "$RDY" & PC=$!
for _ in $(seq 1 30); do [ -e "$RDY" ] && break; sleep 0.1; done
"$BUILD/sim_ctl" "$SOCK" drive 20 1 >/dev/null    # rising edge
wait $PC; rc=$?
assert_eq "poll(rising) woke on 0->1" "0" "$rc"

echo "== EDGE / POLL: does NOT wake on wrong edge =="
"$BUILD/sim_ctl" "$SOCK" reboot >/dev/null
"$BUILD/sim_ctl" "$SOCK" export 21 >/dev/null
"$BUILD/sim_ctl" "$SOCK" direction 21 in >/dev/null
"$BUILD/sim_ctl" "$SOCK" drive 21 1 >/dev/null     # start high
RDY=/tmp/rdy.neg; rm -f "$RDY"
"$BUILD/poll_client" "$MOUNT" 21 rising 1500 "$RDY" & PC=$!
for _ in $(seq 1 30); do [ -e "$RDY" ] && break; sleep 0.1; done
"$BUILD/sim_ctl" "$SOCK" drive 21 0 >/dev/null     # falling edge (wrong)
wait $PC; rc=$?
assert_eq "poll(rising) ignored 1->0 (timeout)" "2" "$rc"

echo "== reboot resets to defaults =="
echo 7 > "$MOUNT/export"
"$BUILD/sim_ctl" "$SOCK" reboot >/dev/null
assert_ok "reboot unexports gpio7" test ! -d "$MOUNT/gpio7"

echo
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ]

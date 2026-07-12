#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# run_examples.sh — run every language example end-to-end against a live simulator,
# mounted at the CANONICAL /sys/class/gpio so unmodified code (onoff, etc.) just works.
#
# For each example: reboot the sim, launch the example (it blinks, then waits for a
# button), and once it prints WAITING, raise GPIO27 via the control socket to simulate
# the press. The example must detect the edge and exit 0.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build"
SYSGPIO=/sys/class/gpio
SOCK=/run/gpio-sim.sock

pass=0; fail=0
ok()  { echo "  ok   - $1"; pass=$((pass+1)); }
bad() { echo "  FAIL - $1"; fail=$((fail+1)); }

# --- make /sys/class/gpio a writable FUSE mountpoint (same tmpfs shadow the
#     container entrypoint uses) ---
setup_canonical() {
    mkdir -p /run/orig-class
    mountpoint -q /run/orig-class || mount --bind /sys/class /run/orig-class
    mount -t tmpfs tmpfs /sys/class
    for e in /run/orig-class/*; do
        ln -s "/run/orig-class/$(basename "$e")" "/sys/class/$(basename "$e")" 2>/dev/null || true
    done
    mkdir -p "$SYSGPIO"
}

cleanup() {
    [ -n "${DP:-}" ] && kill "$DP" 2>/dev/null
    fusermount3 -u "$SYSGPIO" 2>/dev/null
}
trap cleanup EXIT

echo "== preparing canonical /sys/class/gpio mount =="
setup_canonical

echo "== starting simulator =="
GPIO_BOARD=pi4 "$BUILD/gpio-sim" "$SYSGPIO" >/tmp/daemon.log 2>&1 &
DP=$!
for _ in $(seq 1 50); do
    cat "$SYSGPIO/gpiochip0/base" >/dev/null 2>&1 && break; sleep 0.1
done
if ! cat "$SYSGPIO/gpiochip0/base" >/dev/null 2>&1; then
    echo "simulator failed to mount at $SYSGPIO"; cat /tmp/daemon.log; exit 1
fi

# run_example <name> <cmd...>
run_example() {
    local name="$1"; shift
    "$BUILD/sim_ctl" "$SOCK" reboot >/dev/null 2>&1
    rm -f /tmp/ex.log
    ( "$@" ) >/tmp/ex.log 2>&1 &
    local pid=$!
    # wait until the example is set up and blocked on the button
    for _ in $(seq 1 150); do grep -q WAITING /tmp/ex.log 2>/dev/null && break; sleep 0.1; done
    # simulate the button press: raise GPIO27 (rising edge)
    "$BUILD/sim_ctl" "$SOCK" drive 27 1 >/dev/null 2>&1
    wait "$pid"; local rc=$?
    if [ "$rc" -eq 0 ] && grep -q "Button pressed" /tmp/ex.log; then
        ok "$name"
    else
        bad "$name (exit $rc)"
        sed 's/^/        | /' /tmp/ex.log
    fi
}

echo "== examples =="
run_example "shell (echo + read loop)"        sh      "$ROOT/examples/shell/blink-button.sh"
run_example "C (epoll on value)"              "$ROOT/examples/c-epoll/blink-button"
run_example "Node.js (onoff)"                 node    "$ROOT/examples/node-onoff/blink-button.js"
run_example "Python (select.poll)"            python3 "$ROOT/examples/python-sysfs/blink-button.py"
if [ -x "$ROOT/examples/go/blink-button" ]; then
    run_example "Go (select exceptfds)"       "$ROOT/examples/go/blink-button"
else
    echo "  skip - Go (binary not built)"
fi

echo
echo "RESULT(examples): $pass passed, $fail failed"
[ "$fail" -eq 0 ]

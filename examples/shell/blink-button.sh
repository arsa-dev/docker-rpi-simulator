#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# blink-button.sh — the canonical teaching example: drive GPIO from the shell by
# writing the sysfs files, exactly as every "getting started with Pi GPIO" tutorial
# shows. Blinks GPIO17 and waits for GPIO27 to go high.
#
#   LED    = GPIO17 (output)
#   BUTTON = GPIO27 (input, rising edge)
#
# NOTE ON POLLING: POSIX shell has no poll(2), so we watch the button with a short
# read loop. For true edge-triggered poll() fidelity see ../c-epoll or ../python-sysfs.

SYSFS=/sys/class/gpio
LED=17
BUTTON=27

# --- blink the LED ---
[ -d "$SYSFS/gpio$LED" ] || echo "$LED" > "$SYSFS/export"
echo out > "$SYSFS/gpio$LED/direction"

echo "Blinking GPIO$LED..."
i=0
while [ "$i" -lt 6 ]; do
    if [ $((i % 2)) -eq 0 ]; then echo 1 > "$SYSFS/gpio$LED/value"
    else                          echo 0 > "$SYSFS/gpio$LED/value"; fi
    sleep 0.1
    i=$((i + 1))
done
echo 0 > "$SYSFS/gpio$LED/value"

# --- set up the button ---
[ -d "$SYSFS/gpio$BUTTON" ] || echo "$BUTTON" > "$SYSFS/export"
echo in     > "$SYSFS/gpio$BUTTON/direction"
echo rising > "$SYSFS/gpio$BUTTON/edge"

echo "WAITING for button press on GPIO$BUTTON (up to 5s)..."

# Poll the value up to 50 * 0.1s = 5s for a high level.
i=0
pressed=0
while [ "$i" -lt 50 ]; do
    if [ "$(cat "$SYSFS/gpio$BUTTON/value")" = "1" ]; then
        pressed=1
        break
    fi
    sleep 0.1
    i=$((i + 1))
done

# --- clean up ---
echo "$LED"    > "$SYSFS/unexport"
echo "$BUTTON" > "$SYSFS/unexport"

if [ "$pressed" -eq 1 ]; then
    echo "Button pressed! value=1"
    exit 0
else
    echo "timeout: no button press"
    exit 3
fi

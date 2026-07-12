#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""blink-button.py — blink an LED and wait for a button using the sysfs GPIO
interface directly, with Python's standard library only.

Edge detection uses select.poll() with POLLPRI on gpio27/value — the same poll(2)
mechanism a real Raspberry Pi sysfs consumer uses. No third-party packages.

    LED    = GPIO17 (output)
    BUTTON = GPIO27 (input, rising edge)

Note: this deliberately does NOT use RPi.GPIO. RPi.GPIO drives the SoC registers via
/dev/gpiomem, not sysfs, so it cannot work against a sysfs simulator. Direct file I/O
(shown here) or python-periphery's sysfs mode both work.

Run:  python3 blink-button.py
"""
import select
import sys
import time

SYSFS = "/sys/class/gpio"
LED = 17
BUTTON = 27


def write(path, value):
    with open(path, "w") as f:
        f.write(str(value))


def export(pin):
    try:
        write(f"{SYSFS}/export", pin)
    except OSError:
        pass  # already exported


def unexport(pin):
    try:
        write(f"{SYSFS}/unexport", pin)
    except OSError:
        pass


def main():
    # --- blink the LED ---
    export(LED)
    write(f"{SYSFS}/gpio{LED}/direction", "out")
    print(f"Blinking GPIO{LED}...")
    for i in range(6):
        write(f"{SYSFS}/gpio{LED}/value", i % 2)
        time.sleep(0.1)
    write(f"{SYSFS}/gpio{LED}/value", 0)

    # --- set up the button: input, rising-edge interrupt ---
    export(BUTTON)
    write(f"{SYSFS}/gpio{BUTTON}/direction", "in")
    write(f"{SYSFS}/gpio{BUTTON}/edge", "rising")

    value = open(f"{SYSFS}/gpio{BUTTON}/value", "r")
    value.read()  # initial read clears any pending state (rearm)

    poller = select.poll()
    poller.register(value, select.POLLPRI | select.POLLERR)

    print(f"WAITING for button press on GPIO{BUTTON} (up to 5s)...", flush=True)
    events = poller.poll(5000)  # milliseconds

    if events:
        value.seek(0)
        level = value.read().strip()
        print(f"Button pressed! value={level}")
        rc = 0
    else:
        print("timeout: no button press")
        rc = 3

    value.close()
    unexport(LED)
    unexport(BUTTON)
    sys.exit(rc)


if __name__ == "__main__":
    main()

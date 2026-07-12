# Python — blink + button with the standard library

Direct sysfs file I/O plus `select.poll(POLLPRI)` on `gpio27/value` — real edge-triggered
detection, no third-party packages.

```sh
python3 blink-button.py
```

Expected output:

```
Blinking GPIO17...
WAITING for button press on GPIO27 (up to 5s)...
Button pressed! value=1
```

Raise GPIO27 while it waits — click it on the panel, or:

```sh
curl -X POST -d '{"level":1}' http://localhost:8080/api/pin/27
```

## Why not RPi.GPIO?

**RPi.GPIO does not work with this simulator**, and can't: it drives the SoC's GPIO
registers directly through `/dev/gpiomem`, bypassing sysfs entirely. A sysfs simulator
has nothing for it to talk to. Use direct file I/O (shown here) or
[`python-periphery`](https://github.com/vsergeev/python-periphery)'s sysfs GPIO mode —
both go through `/sys/class/gpio`.

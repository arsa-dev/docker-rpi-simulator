# Examples — "blink an LED, read a button" in several languages

Each example does the same two things against the **sysfs** GPIO interface, so it runs
unchanged inside the simulator (and on a real Pi with sysfs enabled):

1. **Blink** an output pin (GPIO17) a few times — watch it flash on the web panel.
2. **Watch** an input pin (GPIO27) for a **rising edge** using `poll()`/`epoll` and print
   when the "button" is pressed.

There is no real button, so you supply the press: click **GPIO27** on the web panel, or
use the control API / CLI. The examples double as the project's integration tests.

| Example | Language | How it talks to GPIO |
|---|---|---|
| [`shell/`](shell/) | POSIX shell | `echo` into `/sys/class/gpio/...`, value-read loop |
| [`node-onoff/`](node-onoff/) | Node.js + [`onoff`](https://www.npmjs.com/package/onoff) | a real-world sysfs library (epoll on `value`) |
| [`python-sysfs/`](python-sysfs/) | Python 3 (stdlib only) | direct file I/O + `select.poll(POLLPRI)` |
| [`c-epoll/`](c-epoll/) | C | `open`/`read`/`write` + `epoll` on `value` |
| [`go/`](go/) | Go (stdlib only) | file I/O + `select(2)` exceptfds (optional) |

## Run them

Start the simulator (panel at http://localhost:8080), then, in the container:

```sh
# shell
sh    /work/examples/shell/blink-button.sh
# node
node  /work/examples/node-onoff/blink-button.js
# python
python3 /work/examples/python-sysfs/blink-button.py
# c
/work/examples/c-epoll/blink-button
```

Each prints `WAITING for button press...`; then raise GPIO27 — on the panel, or:

```sh
curl -X POST -d '{"level":1}' http://localhost:8080/api/pin/27      # via the control API
```

## What works here — and what does not

This simulator emulates the **sysfs** interface (`/sys/class/gpio`). Any code that uses
sysfs works, in any language. The following do **not** work, because they bypass sysfs:

- **RPi.GPIO** and **WiringPi** — they poke the SoC registers directly via `/dev/gpiomem`.
- **libgpiod** / `gpioset`/`gpioget` / modern **gpiozero** — they use the character device
  (`/dev/gpiochipN`), a different kernel mechanism.

Those are on the roadmap as separate device modules. See the top-level `README` and
`docs/adr/0001-architecture.md`.

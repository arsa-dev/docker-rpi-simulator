# ADR 0001 — Architecture of the Dockerized sysfs GPIO simulator

- Status: **Accepted** (Milestone gate 1, 2026-07-12) — stack and mount decisions confirmed by owner
- Date: 2026-07-12
- Deciders: repository owner
- Supersedes: —

> **Review outcome (2026-07-12):** Stack = **C11 + libfuse3** (proven poll path).
> Default mount = **`overlay`** with `plain` fallback. Remaining open items 4 & 5
> (publishing owner/name, license, chip defaults) are non-blocking for Milestone 2
> and tracked below.

---

## Context

We are building a public, open-source Docker image that simulates the Linux GPIO
**sysfs** interface (`/sys/class/gpio/...`) entirely in userspace, so GPIO code written
in any language that talks to sysfs runs unchanged in a container with no Raspberry Pi
and no real hardware. It ships with a web panel (virtual 40-pin header) to observe
output pins and operate input pins, plus runnable examples in several languages.

The single job of the product: **make sysfs GPIO behave like real hardware, visibly.**

The deciding technical constraint is **poll/edge fidelity**. Real code relies on
`poll(2)`/`epoll` blocking on the `value` file until an input line transitions in the
configured `edge` direction, then returning `POLLPRI|POLLERR`. `onoff.watch` (Node),
gpiozero's sysfs path, and hand-rolled C `epoll` loops all depend on this. If our FUSE
layer cannot deliver a true blocking `poll` wakeup, edge detection does not work and the
product fails its core promise. Every stack decision below is subordinate to this.

## Prior art surveyed

| Project | Stack | What it is | Poll/edge fidelity | What we borrow / avoid |
|---|---|---|---|---|
| [`brgl/gpiod-sysfs-proxy`](https://github.com/brgl/gpiod-sysfs-proxy) | Python + FUSE, backed by real libgpiod | Compat layer mounted over `/sys/class/gpio` on real hardware | **Inadequate.** README: "reading from `value` will not block after the value has been read once" and "we currently don't support multiple users polling the `value` attribute at once." | **Borrow:** the overlayfs-over-`/sys/class` mount recipe and `-o allow_other -o default_permissions`. **Avoid:** the Python/high-level binding — it can't block on poll, which is disqualifying for us. |
| [`info-beamer/sysfs-gpio-shim`](https://github.com/info-beamer/sysfs-gpio-shim) | libfuse3 + libgpiod | FUSE emulator of deprecated sysfs on a Pi | "Edge detection by polling `/value` should work" — i.e. it *polls the underlying value*, not a true kernel-style `POLLPRI` wakeup. | **Borrow:** the `gpio`-group permission model — `-o default_permissions,allow_other,gid=$(getent group gpio)`. **Avoid:** value-polling as the edge mechanism (busy, laggy). |
| [`krjakbrjak/gpio-sysfs-simulator`](https://github.com/krjakbrjak/gpio-sysfs-simulator) ([DEV.to writeup](https://dev.to/krjakbrjak/simulating-gpio-sysfs-interface-with-fuse-and-c-30ga)) | **C++ + libfuse, CMake** | **A pure simulator** (no hardware backing): dynamic export/unexport, implements the FUSE `.poll` op so `epoll` clients are notified on change. | **Proven.** Closest existing thing to our goal and the reference that establishes C++/libfuse can deliver real poll notifications. | **Borrow heavily:** overall shape (pure sim, dynamic tree, `.poll` + notify). **Extend:** authoritative in-memory line model shared with a web server; edge-direction filtering; 40-pin panel. |
| Kernel [`Documentation/gpio/sysfs.txt`](https://www.kernel.org/doc/Documentation/gpio/sysfs.txt) | — | Authoritative semantics | — | Ground truth for `export`/`unexport`, `direction` incl. glitch-free `high`/`low`, `value`, `edge` (`none`/`rising`/`falling`/`both`, `POLLPRI|POLLERR`), `active_low` inversion, `gpiochipN` `base`/`label`/`ngpio`. |
| Kernel [`gpio-sim` (configfs)](https://www.kernel.org/doc/html/latest/admin-guide/gpio/gpio-sim.html) | in-kernel module | The in-kernel simulator | Real chardev semantics | **Rejected as a dependency** — see below. |

### Why we are *not* relying on the in-kernel `gpio-sim`

`gpio-sim` is configured via **configfs** (`/config/gpio-sim/...`) and requires the
`gpio-sim` **kernel module** loaded plus configfs mounted. In a plain container the
module generally isn't present, configfs isn't available/writable, and loading modules
needs host privileges. On **macOS Docker Desktop** the LinuxKit VM ships a minimal
kernel that does not provide the module. It also only exposes the **character device**
interface, not sysfs. A userspace FUSE simulator is the *entire reason this project
exists*: it needs no special kernel, no module, and runs on macOS Docker Desktop with
FUSE-only capabilities.

### The libfuse poll mechanism we will use (the linchpin)

libfuse3 exposes, in `fuse_operations`:

```c
int (*poll)(const char *path, struct fuse_file_info *fi,
            struct fuse_pollhandle *ph, unsigned *reventsp);
```

When a client `poll()`s a FUSE-backed file, the kernel FUSE driver calls our `.poll`
with a `struct fuse_pollhandle *ph`. We **store `ph`** against that pin's `value` file
and return current readiness in `*reventsp`. When the pin's line later changes **in the
configured edge direction**, we call **`fuse_notify_poll(ph)`**, which makes the kernel
re-issue `.poll`; we then return `POLLPRI|POLLERR` in `*reventsp`, exactly matching
kernel sysfs. A single notification clears all pending waiters. This is the mechanism the
`krjakbrjak` reference validates, and it is why the binding choice below is constrained.

## Decision

### 1. Language / stack: **C (C11) single binary, libfuse3 high-level API**

- **FUSE layer:** libfuse3, high-level API, implementing `.poll` + `fuse_notify_poll`
  as above. This is the only option among the surveyed references *proven* to deliver
  blocking, edge-filtered poll wakeups. C keeps us aligned with the one working
  reference and with the canonical FUSE examples, and yields a small static-ish binary.
- **Go / Rust rejected for now, not forever.** The brief requires proving the binding
  exposes FUSE poll notifications *before* committing. `go-fuse` and Rust's `fuser` do
  not clearly surface `FUSE_POLL`/`FUSE_NOTIFY_POLL` in their high-level trait APIs; at
  minimum each needs a spike. Rather than gamble the core feature on an unverified
  binding, we take the proven C path. (If someone lands a verified poll spike in Go/Rust
  later, revisiting is cheap — the line model is language-independent.)

### 2. Single process = single source of truth

One daemon owns an in-memory **line model** (the authoritative pin state) and runs two
frontends against it:

1. the **FUSE filesystem** mounted at `/sys/class/gpio`, and
2. an **HTTP + WebSocket server** for the panel and a JSON control API.

A CLI `echo 1 > value`, an `onoff` call, and a panel click all mutate the *same* model
under a mutex. Every mutation fans out to (a) `fuse_notify_poll` for any waiting poller
whose edge matches, and (b) a WebSocket broadcast to connected panels. This is the
brief's recommended architecture and it eliminates the state-sync bugs a two-process
design would invite (the Python proxy's poll limitations partly stem from not owning a
unified model).

- **Web server library:** an embedded, permissively licensed C HTTP+WebSocket library so
  everything stays in one process and one binary. Primary candidate: **civetweb** (MIT,
  supports HTTP and WebSocket, embeddable). Alternatives if it disappoints:
  libwebsockets (MIT) or Mongoose (GPLv2 — rejected on license grounds for an MIT repo).
  Final pick to be confirmed during Milestone 4; the abstraction below keeps it swappable.
- Panel assets are **self-contained** (no CDN/fonts) and served by the same process.

### 3. Modular "device" architecture so chardev/i2c are future siblings

The core is a **line model + event bus**; the sysfs tree is *one frontend* onto it, not
the whole program. Concretely:

```
                 +------------------- daemon (PID 1) -------------------+
                 |                                                      |
  sysfs FUSE  ---+--> [ frontend: sysfs ] --\                          |
                 |                            >-- [ LINE MODEL ] --<-- event bus
  HTTP/WS API ---+--> [ frontend: web    ] --/         ^               |
                 |                                       |             |
  (future)    ---+--> [ frontend: chardev ] ------------/             |
  (future)    ---+--> [ frontend: i2c     ]                           |
                 +------------------------------------------------------+
```

- **Line model**: pins as physical lines with `direction`, `value`, `edge`,
  `active_low`, export state, and a list of poll waiters. Only this touches state.
- **Event bus**: on any line change, notifies subscribed frontends. sysfs frontend maps
  a subscription to `fuse_notify_poll`; web frontend maps it to a WS broadcast.
- A future **chardev** frontend (libgpiod / `/dev/gpiochipN` via CUSE) and an **i2c**
  frontend become new subscribers on the same model with **no rework** of the core.
  This satisfies the modularity constraint explicitly.

### 4. Container mount strategy: overlay-over-`/sys/class`, with a plain-path fallback

The crux is that `/sys/class/gpio` usually **doesn't exist** in a plain container / on
macOS Docker Desktop and `/sys` is read-only, so we can't mount straight onto it.

- **Primary mode (`overlay`)** — adapts the `gpiod-sysfs-proxy` recipe so *unmodified*
  libraries find the sim at the canonical path:
  1. `mount -t sysfs sysfs /run/gpio/sys`
  2. `mount -t overlay overlay -o lowerdir=/run/gpio/sys/class,upperdir=/run/gpio/class,workdir=/run/gpio/work /sys/class` (writable upper supplies a `gpio/` dir)
  3. FUSE-mount the simulator at `/sys/class/gpio`.
  Result: `onoff`, `python-periphery`, shell, and C examples work at the real path
  with zero configuration.
- **Fallback mode (`plain`)** — FUSE-mount the sim at a plain writable path (`/gpio`)
  for environments where overmounting `/sys` is impossible. Tooling that accepts a
  configurable base (e.g. an env var / a symlink target) is pointed there; documented
  clearly, with the caveat that libraries hardcoding `/sys/class/gpio` need `overlay`.
- Entrypoint auto-detects: if `/sys/class/gpio` already exists (real Pi), don't
  overmount; else try `overlay`; else fall back to `plain` and print how to target it.

### 5. Minimal privileges (never `--privileged`)

Required, and *only* these:

- `--device /dev/fuse` — the FUSE char device the daemon opens to mount.
- `--cap-add SYS_ADMIN` — `mount(2)` for the FUSE mount and the overlay/sysfs mounts.
- `--security-opt apparmor=unconfined` — Docker's default apparmor profile denies
  `mount`, which would block both the overlay and FUSE mounts.

Each is documented in the README with this justification. `plain` fallback needs only
`/dev/fuse` + `SYS_ADMIN` (no overlay), which helps locked-down environments.

### 6. Base image & packaging

- **`debian:bookworm-slim`** (glibc). libfuse3 and the FUSE3 userspace (`fuse3`) are
  first-class Debian packages; glibc avoids musl/FUSE edge cases; multi-arch images are
  straightforward. Alpine was considered for size but rejected to avoid musl/FUSE and
  glibc-only example toolchains complicating the "any language" promise.
- Multi-stage build → small final image; multi-arch (`linux/amd64` + `linux/arm64`) via
  `docker buildx`. Entrypoint runs setup as root, mounts, prints a banner with the panel
  URL, then `exec`s the daemon as PID 1 (proper signal handling, clean unmount on
  SIGTERM).
- Config via env vars (panel port, chip `base`/`ngpio`/`label`, board model, log level,
  mount mode) + an optional mounted **scenario file** scripting input transitions over
  time for demos and tests.

## Consequences

**Positive**
- Poll/edge fidelity is designed in from the start on a *proven* mechanism, not hoped
  for. Edge detection — the feature that makes this useful — works for real consumers.
- One process, one source of truth → CLI, library, panel, and control API can never
  disagree.
- The model/frontend split makes chardev and i2c additive, honoring the modularity
  constraint without speculative code now.
- Runs on macOS Docker Desktop and Linux with FUSE-only caps; honest, documented scope.

**Negative / risks**
- C for the web/WebSocket layer is more effort than a higher-level language. Mitigation:
  a small embedded lib (civetweb) and a thin JSON API; the panel is static.
- civetweb/libwebsockets choice is not yet battle-tested in this repo — confirmed at
  Milestone 4 behind the frontend abstraction, so a swap is contained.
- Overlaying `/sys/class` is a non-obvious trick and can surprise users; it will be
  heavily commented and documented, with the `plain` fallback as the escape hatch.
- The `plain` fallback does **not** help libraries that hardcode `/sys/class/gpio`;
  stated plainly in docs.

## Scope honesty (to be mirrored in the README)

This simulates the **sysfs** interface. Code using `/sys/class/gpio` (any language)
works. Code using `/dev/gpiomem` register access (**RPi.GPIO**, **WiringPi**) or the
**libgpiod character device** (`/dev/gpiochipN`, `gpioset`/`gpioget`, modern gpiozero)
does **not** — those are on the roadmap as separate device modules. We will **not**
claim RPi.GPIO support, because it bypasses sysfs via `/dev/gpiomem`.

## Open questions — resolved

1. **Stack:** ✅ **C + libfuse3.** Poll fidelity since proven end-to-end on real FUSE
   (Milestone 2: `epoll(value)` wakes with `POLLPRI|POLLERR` on the matching edge only).
2. **Web library:** deferred to Milestone 4 behind the frontend abstraction. Note: the
   project is **copyleft (GPL-3.0-or-later)**, so GPL web/WebSocket libraries (e.g.
   Mongoose) are now license-compatible — the ADR's earlier MIT-driven rejection of them
   no longer applies.
3. **Default mount mode:** ✅ **auto → canonical, plain fallback.** See the decision-#4
   update below: on Docker Desktop's kernel, overlayfs over `/sys/class` is rejected
   (sysfs lowerdir + nested-overlay/tmpfs upperdir both unsupported). The working
   equivalent is a **tmpfs shadow** of `/sys/class` that preserves the other class
   entries via symlinks, then a FUSE mount at `/sys/class/gpio`. Verified on macOS
   Docker Desktop.
4. **Publishing / license:** ✅ repo **`arsa-dev/docker-rpi-simulator`** →
   `ghcr.io/arsa-dev/docker-rpi-simulator`; license **GPL-3.0-or-later** (copyleft, per
   owner). AGPL-3.0 remains an option if network-use copyleft is wanted for the panel.
5. **Chip defaults:** ✅ `base=0`, `ngpio=28` (BCM 0–27), default `label=pinctrl-bcm2835`.
   Added **board presets** via `GPIO_BOARD`: `classic` (bcm2835, default), `pi4`
   (bcm2711), `pi5` (rp1). All keep `base=0` so sysfs `gpioN == BCM N`.

### Decision #4 update (Milestone 3): tmpfs shadow instead of overlayfs

The overlayfs recipe from `gpiod-sysfs-proxy` does not work on Docker Desktop's
LinuxKit kernel: sysfs is not accepted as an overlay `lowerdir`, and the only writable
`upperdir` available is on the nested container overlay/tmpfs, which is also
unsupported. The implemented `canonical` mode instead:
1. `mount --bind /sys/class /run/orig-class` (preserve the real entries),
2. `mount -t tmpfs tmpfs /sys/class` (writable shadow),
3. symlink the originals back in (`net`, `block`, …),
4. `mkdir /sys/class/gpio` and FUSE-mount the simulator there.
This satisfies the same goal — unmodified libraries find the sim at the canonical
`/sys/class/gpio` — with the same minimal caps (`SYS_ADMIN` + `apparmor=unconfined`),
never `--privileged`. The `plain` mode (mount at `/gpio`) remains the fallback, and
`auto` degrades to it gracefully when the tmpfs shadow can't be set up.

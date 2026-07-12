# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Known follow-ups
- Pin the `debian:bookworm-slim` base image to a dated tag / digest for reproducibility.
- Character-device (`/dev/gpiochipN`) and I²C device modules (roadmap).

## [0.1.0] - 2026-07-12

First release: a working userspace **sysfs** GPIO simulator with a web panel.

### Added
- **FUSE sysfs core** emulating `/sys/class/gpio`: `export`/`unexport`,
  `direction` (incl. glitch-free `high`/`low`), `value`, `edge`, `active_low`, and
  `gpiochipN` attributes — with kernel-matching errno behaviour.
- **Real poll/edge fidelity**: `poll(2)`/`epoll` on `value` blocks until the configured
  edge fires and returns `POLLPRI|POLLERR`, via libfuse `.poll` + `fuse_notify_poll`.
- **Single authoritative line model** shared by all frontends (sysfs, web, control
  socket), so CLI, library, and panel never disagree.
- **Web panel**: faithful 40-pin header, outputs that glow when high, clickable input
  switches, live WebSocket updates, "simulate reboot", keyboard-accessible, self-contained.
- **Control API**: `GET /api/state`, `POST /api/pin/{n}`, `POST /api/reboot`, WebSocket.
- **Container**: entrypoint with `auto`/`canonical`/`plain` mount modes — canonical makes
  the sim appear at the real `/sys/class/gpio` via a tmpfs shadow of `/sys/class` — using
  only FUSE capabilities, never `--privileged`. Verified on Linux and macOS Docker Desktop.
- **Board presets** via `GPIO_BOARD`: `classic` (bcm2835), `pi4` (bcm2711), `pi5` (rp1).
- **Examples** (runnable, and used as integration tests) in shell, C (`epoll`),
  Node.js (`onoff`), Python (`select.poll`), and Go (`select`).
- **CI**: native unit tests + `shellcheck` + core/web integration on every PR; language
  examples on a schedule/manual trigger; multi-arch (`amd64`/`arm64`) GHCR publish with
  SBOM + provenance on version tags.

### Documented limitations
- Simulates **sysfs only**. `RPi.GPIO` and `WiringPi` (`/dev/gpiomem`) and libgpiod /
  character-device tools (`/dev/gpiochipN`) are **not** supported — those are on the roadmap.

[Unreleased]: https://github.com/arsa-dev/docker-rpi-simulator/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/arsa-dev/docker-rpi-simulator/releases/tag/v0.1.0

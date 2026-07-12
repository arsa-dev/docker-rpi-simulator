# Contributing

Thanks for your interest! This is a learning-focused project — correctness and honesty
about what the simulator does (and doesn't) matter more than breadth.

## Ground rules

- **Scope honesty.** Don't claim support for anything that bypasses sysfs (RPi.GPIO,
  WiringPi, libgpiod/chardev) unless it's actually a new device module. If in doubt, ask.
- **Minimal privileges.** The container must keep working with only
  `--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined`. Never
  introduce a `--privileged` requirement.
- **Keep the model authoritative.** All pin state lives in `src/core`. Frontends (sysfs,
  web, and future chardev/i2c) are subscribers — don't add a second source of truth.
- Small, reviewable commits. Explain non-obvious FUSE/mount tricks in comments.

## Building and testing

```sh
make test                                    # native unit tests (no Docker, any OS)
./tests/integration/run_docker_it.sh         # core + web suites on real FUSE
./tests/integration/run_docker_examples.sh   # all language examples end-to-end
```

The daemon links libfuse3, so it builds on Linux (or in Docker). The core model is
portable C and unit-tests anywhere, including macOS.

CI runs `make test`, `shellcheck`, and the core + web integration suites on every PR. The
language-examples suite runs on a schedule / manual dispatch (it's heavier).

## Architecture

Read [`docs/adr/0001-architecture.md`](docs/adr/0001-architecture.md) first — it explains
the single-process line-model design, why C + libfuse3, the poll/edge mechanism, and the
container mount strategy. New device interfaces should be added as sibling frontends
following that pattern, each with its own ADR.

## Licensing

By contributing you agree your work is licensed under **AGPL-3.0-or-later**. Add an SPDX
header to new source files:

```
SPDX-License-Identifier: AGPL-3.0-or-later
```

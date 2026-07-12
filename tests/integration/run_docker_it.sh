#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# run_docker_it.sh — build the dev image and run the FUSE integration suite.
#
# Works on Linux and macOS Docker Desktop. Uses only FUSE capabilities, never
# --privileged (ADR 0001, decision #5).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="${IMAGE:-gpio-sim-dev}"

echo "== building $IMAGE =="
docker build -f "$ROOT/docker/Dockerfile.dev" -t "$IMAGE" "$ROOT"

echo "== running integration suite =="
exec docker run --rm \
    --device /dev/fuse \
    --cap-add SYS_ADMIN \
    --security-opt apparmor=unconfined \
    "$IMAGE"

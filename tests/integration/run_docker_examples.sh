#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# run_docker_examples.sh — build the examples image and run every language example
# end-to-end against a live simulator. FUSE caps only, never --privileged.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="${IMAGE:-gpio-sim-examples}"

echo "== building $IMAGE =="
docker build -f "$ROOT/docker/Dockerfile.examples" -t "$IMAGE" "$ROOT"

echo "== running language examples =="
exec docker run --rm \
    --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined \
    "$IMAGE"

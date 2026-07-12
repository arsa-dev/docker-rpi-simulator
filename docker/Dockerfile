# Dockerfile — production image for the sysfs GPIO simulator.
#
# Multi-stage: a builder compiles the static-ish daemon; the runtime image is
# debian:bookworm-slim plus only the FUSE userspace it needs. Multi-arch
# (linux/amd64 + linux/arm64) via `docker buildx` (wired up in Milestone 6).
#
# Run (never --privileged):
#   docker run --rm -it \
#     --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined \
#     ghcr.io/arsa-dev/docker-rpi-simulator

# ---- builder ------------------------------------------------------------
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config libfuse3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src
RUN make daemon && strip build/gpio-sim

# ---- runtime ------------------------------------------------------------
FROM debian:bookworm-slim

LABEL org.opencontainers.image.title="docker-rpi-simulator" \
      org.opencontainers.image.description="Userspace FUSE simulator of the Linux sysfs GPIO interface, with a web panel." \
      org.opencontainers.image.source="https://github.com/arsa-dev/docker-rpi-simulator" \
      org.opencontainers.image.licenses="AGPL-3.0-or-later"

# fuse3 provides fusermount3 + mount helpers; libfuse3-3 is the runtime lib.
# util-linux (mount/umount/mountpoint) is already in the base image.
RUN apt-get update && apt-get install -y --no-install-recommends \
        fuse3 libfuse3-3 \
    && rm -rf /var/lib/apt/lists/* \
    && echo "user_allow_other" >> /etc/fuse.conf

COPY --from=builder /src/build/gpio-sim /usr/local/bin/gpio-sim
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
COPY web/ /usr/local/share/gpio-sim/web/
RUN chmod +x /usr/local/bin/entrypoint.sh

# Sensible defaults; override at `docker run` time.
ENV GPIO_BOARD=classic \
    GPIO_MOUNT_MODE=auto \
    GPIO_MOUNT=/gpio \
    GPIO_CONTROL_SOCK=/run/gpio-sim.sock \
    GPIO_WEB_ROOT=/usr/local/share/gpio-sim/web \
    GPIO_HTTP_PORT=8080 \
    GPIO_LOG_LEVEL=info

# Web panel port (served from Milestone 4).
EXPOSE 8080

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

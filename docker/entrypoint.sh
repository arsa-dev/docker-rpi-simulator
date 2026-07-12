#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# entrypoint.sh — set up the mount, then hand off to the daemon as PID 1.
#
# The crux (ADR 0001, decision #4): in a plain container and on macOS Docker
# Desktop, /sys/class/gpio does not exist and /sys is read-only, so we can't just
# mount onto it — and we need unmodified libraries (onoff, python-periphery, shell
# tutorials) to find the sim at that canonical path.
#
# We tried overlayfs (the gpiod-sysfs-proxy recipe) but Docker Desktop's LinuxKit
# kernel rejects it two ways: sysfs is unsupported as an overlay lowerdir, and the
# only writable upperdir available sits on the nested container overlay/tmpfs, also
# unsupported. The reliable equivalent that DOES work there is a tmpfs shadow:
#
#   1. bind the real /sys/class aside (so its entries aren't lost),
#   2. mount a writable tmpfs over /sys/class,
#   3. symlink the original entries back in (net, block, ...),
#   4. mkdir /sys/class/gpio and FUSE-mount the simulator on it.
#
# A `plain` fallback mounts at /gpio for environments where even that is blocked.
#
# Required container flags (never --privileged):
#   --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined
#
# GPIO_MOUNT_MODE = auto (default) | canonical | plain   (overlay = alias of canonical)
set -euo pipefail

SYS_CLASS="/sys/class"
SYS_GPIO="/sys/class/gpio"
ORIG_CLASS="/run/orig-class"
PLAIN_MNT="${GPIO_MOUNT:-/gpio}"
MODE="${GPIO_MOUNT_MODE:-auto}"
[ "$MODE" = "overlay" ] && MODE="canonical"   # backward-compatible alias

log() { echo "[entrypoint] $*"; }

if [ ! -e /dev/fuse ]; then
    log "ERROR: /dev/fuse is missing. Run with:  --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined"
    exit 1
fi

# Make /sys/class/gpio exist as a writable FUSE mountpoint via a tmpfs shadow that
# preserves the rest of /sys/class. Returns 0 on success, 1 if the kernel won't allow it.
setup_canonical() {
    # Already usable (re-run in the same namespace, or a real Pi kernel provides it).
    if mountpoint -q "$SYS_GPIO" 2>/dev/null; then return 0; fi
    if [ -d "$SYS_GPIO" ] && [ -w "$SYS_GPIO" ]; then return 0; fi

    mkdir -p "$ORIG_CLASS"
    if ! mountpoint -q "$ORIG_CLASS"; then
        mount --bind "$SYS_CLASS" "$ORIG_CLASS" 2>/tmp/mnt.err || { log "bind /sys/class: $(cat /tmp/mnt.err)"; return 1; }
    fi
    mount -t tmpfs tmpfs "$SYS_CLASS" 2>/tmp/mnt.err || { log "tmpfs /sys/class: $(cat /tmp/mnt.err)"; return 1; }

    # Re-expose the original /sys/class entries through the preserved bind.
    for e in "$ORIG_CLASS"/*; do
        [ -e "$e" ] || continue
        ln -s "$ORIG_CLASS/$(basename "$e")" "$SYS_CLASS/$(basename "$e")" 2>/dev/null || true
    done
    mkdir -p "$SYS_GPIO" || return 1
    return 0
}

TARGET=""
case "$MODE" in
    plain)
        mkdir -p "$PLAIN_MNT"; TARGET="$PLAIN_MNT"
        log "plain mode: simulator at $TARGET"
        log "NOTE: libraries that hardcode $SYS_GPIO won't find it here — point tools at $TARGET"
        ;;
    canonical)
        if ! setup_canonical; then
            log "ERROR: canonical mode requested but the tmpfs shadow of /sys/class failed."
            exit 1
        fi
        TARGET="$SYS_GPIO"
        log "canonical mode: simulator at $TARGET (unmodified libraries just work)"
        ;;
    auto)
        if setup_canonical; then
            TARGET="$SYS_GPIO"
            log "auto: canonical path ready — simulator at $TARGET"
        else
            mkdir -p "$PLAIN_MNT"; TARGET="$PLAIN_MNT"
            log "auto: canonical setup unavailable — falling back to plain mode at $TARGET"
            log "NOTE: point tools at $TARGET (libraries hardcoding $SYS_GPIO need canonical mode)"
        fi
        ;;
    *)
        log "ERROR: unknown GPIO_MOUNT_MODE='$MODE' (want auto|canonical|plain)"; exit 1
        ;;
esac

# exec so the daemon becomes PID 1 and receives SIGTERM/SIGINT directly; libfuse
# unmounts the FUSE filesystem cleanly on those signals. The tmpfs/bind mounts live
# in the container's mount namespace and vanish when the container stops.
export GPIO_MOUNT="$TARGET"
log "starting daemon (board=${GPIO_BOARD:-classic}, mount=$TARGET)"
exec gpio-sim "$TARGET"

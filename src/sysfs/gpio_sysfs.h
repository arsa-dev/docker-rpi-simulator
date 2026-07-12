/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * gpio_sysfs.h — the sysfs FUSE frontend.
 *
 * Presents the model as a `/sys/class/gpio`-shaped tree and, crucially, wires the
 * `value` file's poll()/epoll() up to real edge notifications via libfuse's
 * `.poll` op + fuse_notify_poll() (ADR 0001 — the linchpin).
 */
#ifndef GPIO_SYSFS_H
#define GPIO_SYSFS_H

#include "gpio_model.h"

/*
 * Mount the sysfs tree for `model` at `mountpoint` and serve it (blocking) until
 * the filesystem is unmounted or the process is signalled. libfuse installs the
 * SIGINT/SIGTERM/SIGHUP handlers that unmount cleanly.
 *
 *   foreground     — run in the foreground (do not daemonize)
 *   debug          — enable libfuse debug tracing
 *   allow_other    — add `-o allow_other,default_permissions` (needed so tools run
 *                    by other users, and the overlay path, can reach the files)
 *
 * Returns the fuse_main() exit status (0 on clean unmount).
 */
int gpio_sysfs_run(gpio_model *model, const char *mountpoint,
                   int foreground, int debug, int allow_other);

#endif /* GPIO_SYSFS_H */

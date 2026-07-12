/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * poll_client.c — proves FUSE poll/edge fidelity end-to-end on real Linux FUSE.
 *
 * Mimics a real sysfs consumer (onoff / a C epoll loop): it configures a line via
 * the sysfs files themselves (export, direction=in, edge=X), then blocks in
 * epoll() on gpioN/value waiting for EPOLLPRI. When it has finished setup and is
 * about to block, it creates <readyfile> so the test harness knows it is safe to
 * fire an edge from the control socket without racing setup.
 *
 * Exit codes:  0 = got the edge   2 = timed out (no edge)   1 = setup error
 *
 * Usage: poll_client <mount> <gpio> <edge> <timeout_ms> <readyfile>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static int write_file(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -errno;
    ssize_t n = write(fd, val, strlen(val));
    int e = (n < 0) ? -errno : 0;
    close(fd);
    return e;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s <mount> <gpio> <edge> <timeout_ms> <readyfile>\n", argv[0]);
        return 1;
    }
    const char *mount   = argv[1];
    int         gpio    = atoi(argv[2]);
    const char *edge    = argv[3];
    int         timeout = atoi(argv[4]);
    const char *ready   = argv[5];

    char p[256], num[16];
    snprintf(num, sizeof(num), "%d", gpio);

    /* export (ignore EBUSY: already exported) */
    snprintf(p, sizeof(p), "%s/export", mount);
    int rc = write_file(p, num);
    if (rc && rc != -EBUSY) { fprintf(stderr, "export: %s\n", strerror(-rc)); return 1; }

    snprintf(p, sizeof(p), "%s/gpio%d/direction", mount, gpio);
    if ((rc = write_file(p, "in"))) { fprintf(stderr, "direction: %s\n", strerror(-rc)); return 1; }

    snprintf(p, sizeof(p), "%s/gpio%d/edge", mount, gpio);
    if ((rc = write_file(p, edge))) { fprintf(stderr, "edge: %s\n", strerror(-rc)); return 1; }

    snprintf(p, sizeof(p), "%s/gpio%d/value", mount, gpio);
    int vfd = open(p, O_RDONLY);
    if (vfd < 0) { fprintf(stderr, "open value: %s\n", strerror(errno)); return 1; }

    /* Clear any initial readiness by reading once (kernel-style rearm). */
    char c[8];
    lseek(vfd, 0, SEEK_SET);
    (void)read(vfd, c, sizeof(c));

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLPRI | EPOLLERR, .data.fd = vfd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, vfd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl: %s\n", strerror(errno));
        return 1;
    }

    /* Signal the harness that we are set up and about to block. */
    int rfd = open(ready, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (rfd >= 0) close(rfd);

    struct epoll_event out;
    int nfd = epoll_wait(ep, &out, 1, timeout);
    if (nfd == 0) {
        printf("TIMEOUT\n");
        return 2;
    }
    if (nfd < 0) {
        fprintf(stderr, "epoll_wait: %s\n", strerror(errno));
        return 1;
    }

    /* Rearm and report the value that triggered us. */
    lseek(vfd, 0, SEEK_SET);
    ssize_t n = read(vfd, c, sizeof(c) - 1);
    if (n > 0) c[n] = '\0'; else strcpy(c, "?");
    for (char *s = c; *s; s++) if (*s == '\n') { *s = '\0'; break; }
    printf("EDGE value=%s events=0x%x\n", c, out.events);
    return 0;
}

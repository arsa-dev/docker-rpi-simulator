/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * sim_ctl.c — minimal control-socket client for tests and scripts.
 *
 * Sends its arguments as one command line to the daemon's control socket, prints
 * the reply, and exits 0 iff the reply begins with "ok".
 *
 * Usage: sim_ctl <sockpath> <cmd> [args...]
 *   e.g. sim_ctl /run/gpio-sim.sock drive 17 1
 */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <sockpath> <cmd> [args...]\n", argv[0]);
        return 2;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 2; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", argv[1]);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 2;
    }

    char line[512] = {0};
    for (int i = 2; i < argc; i++) {
        strncat(line, argv[i], sizeof(line) - strlen(line) - 2);
        strncat(line, i + 1 < argc ? " " : "\n", sizeof(line) - strlen(line) - 1);
    }
    if (write(fd, line, strlen(line)) < 0) { perror("write"); return 2; }

    char reply[256];
    ssize_t n = read(fd, reply, sizeof(reply) - 1);
    if (n <= 0) { fprintf(stderr, "no reply\n"); return 2; }
    reply[n] = '\0';
    fputs(reply, stdout);
    return strncmp(reply, "ok", 2) == 0 ? 0 : 1;
}

/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "control_sock.h"
#include "gpio_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct control_sock {
    gpio_model     *model;
    int             fd;        /* listening socket                 */
    char            path[108]; /* sun_path is 108 bytes on Linux   */
    pthread_t       thread;
    volatile int    running;
};

/* Send a NUL-terminated reply. */
static void reply(int fd, const char *s)
{
    (void)write(fd, s, strlen(s));
}

/* Translate a model return code to a reply line. */
static void reply_rc(int fd, int rc)
{
    if (rc == 0) reply(fd, "ok\n");
    else {
        char b[32];
        snprintf(b, sizeof(b), "err %d\n", -rc);
        reply(fd, b);
    }
}

/* Handle one command line. `line` is mutable and NUL-terminated (no newline). */
static void dispatch(control_sock *cs, int fd, char *line)
{
    char cmd[24] = {0};
    int  n = 0, v = 0;
    char arg[24] = {0};

    if (sscanf(line, "%23s", cmd) != 1)
        return;

    if      (!strcmp(cmd, "ping"))   { reply(fd, "ok\n"); }
    else if (!strcmp(cmd, "reboot")) { gpio_model_reboot(cs->model); reply(fd, "ok\n"); }
    else if (!strcmp(cmd, "drive")) {
        if (sscanf(line, "%*s %d %d", &n, &v) == 2)
            reply_rc(fd, gpio_model_drive_line(cs->model, n, v));
        else reply(fd, "err 22\n");
    }
    else if (!strcmp(cmd, "read")) {
        if (sscanf(line, "%*s %d", &n) == 1) {
            int rc = gpio_model_read_value(cs->model, n);
            if (rc < 0) reply_rc(fd, rc);
            else { char b[16]; snprintf(b, sizeof(b), "ok %d\n", rc); reply(fd, b); }
        } else reply(fd, "err 22\n");
    }
    else if (!strcmp(cmd, "export")) {
        if (sscanf(line, "%*s %d", &n) == 1) reply_rc(fd, gpio_model_export(cs->model, n));
        else reply(fd, "err 22\n");
    }
    else if (!strcmp(cmd, "unexport")) {
        if (sscanf(line, "%*s %d", &n) == 1) reply_rc(fd, gpio_model_unexport(cs->model, n));
        else reply(fd, "err 22\n");
    }
    else if (!strcmp(cmd, "direction")) {
        if (sscanf(line, "%*s %d %23s", &n, arg) == 2)
            reply_rc(fd, gpio_model_set_direction(cs->model, n, arg));
        else reply(fd, "err 22\n");
    }
    else if (!strcmp(cmd, "edge")) {
        gpio_edge_t e;
        if (sscanf(line, "%*s %d %23s", &n, arg) == 2 && gpio_edge_from_str(arg, &e) == 0)
            reply_rc(fd, gpio_model_set_edge(cs->model, n, e));
        else reply(fd, "err 22\n");
    }
    else if (!strcmp(cmd, "value")) {
        if (sscanf(line, "%*s %d %d", &n, &v) == 2)
            reply_rc(fd, gpio_model_write_value(cs->model, n, v));
        else reply(fd, "err 22\n");
    }
    else if (!strcmp(cmd, "active_low")) {
        if (sscanf(line, "%*s %d %d", &n, &v) == 2)
            reply_rc(fd, gpio_model_set_active_low(cs->model, n, v != 0));
        else reply(fd, "err 22\n");
    }
    else {
        reply(fd, "err 22\n");
    }
}

/* Serve one connection: read commands line-by-line until the peer closes. */
static void serve(control_sock *cs, int fd)
{
    char   buf[512];
    size_t used = 0;
    ssize_t r;
    while ((r = read(fd, buf + used, sizeof(buf) - 1 - used)) > 0) {
        used += (size_t)r;
        buf[used] = '\0';
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', (buf + used) - start)) != NULL) {
            *nl = '\0';
            dispatch(cs, fd, start);
            start = nl + 1;
        }
        /* Shift any partial line to the front. */
        size_t rem = (buf + used) - start;
        memmove(buf, start, rem);
        used = rem;
        if (used >= sizeof(buf) - 1) used = 0;   /* overlong line: drop */
    }
}

static void *accept_loop(void *arg)
{
    control_sock *cs = arg;
    while (cs->running) {
        int c = accept(cs->fd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;   /* socket closed by control_sock_stop */
        }
        serve(cs, c);
        close(c);
    }
    return NULL;
}

control_sock *control_sock_start(gpio_model *model, const char *path)
{
    control_sock *cs = calloc(1, sizeof(*cs));
    if (!cs)
        return NULL;
    cs->model = model;
    snprintf(cs->path, sizeof(cs->path), "%s", path);

    cs->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cs->fd < 0) { LOG_ERR("control: socket: %s", strerror(errno)); free(cs); return NULL; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);   /* clear any stale socket */

    if (bind(cs->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("control: bind %s: %s", path, strerror(errno));
        close(cs->fd); free(cs); return NULL;
    }
    if (listen(cs->fd, 8) < 0) {
        LOG_ERR("control: listen: %s", strerror(errno));
        close(cs->fd); unlink(path); free(cs); return NULL;
    }

    cs->running = 1;
    if (pthread_create(&cs->thread, NULL, accept_loop, cs) != 0) {
        LOG_ERR("control: pthread_create failed");
        close(cs->fd); unlink(path); free(cs); return NULL;
    }
    LOG_INFO("control: listening on %s", path);
    return cs;
}

void control_sock_stop(control_sock *cs)
{
    if (!cs)
        return;
    cs->running = 0;
    shutdown(cs->fd, SHUT_RDWR);   /* break a blocked accept() */
    close(cs->fd);
    pthread_join(cs->thread, NULL);
    unlink(cs->path);
    free(cs);
}

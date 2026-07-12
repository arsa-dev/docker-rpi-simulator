/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * blink-button.c — blink an LED and wait for a button, using the sysfs GPIO
 * interface directly with open/read/write + epoll. This is the reference for
 * poll fidelity: epoll on gpioN/value with EPOLLPRI blocks until the input's
 * configured edge fires, exactly as on a real Raspberry Pi.
 *
 *   LED    = GPIO17 (output)
 *   BUTTON = GPIO27 (input, rising edge)
 *
 * Build:  make          (or: cc -O2 -o blink-button blink-button.c)
 * Run:    ./blink-button
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define SYSFS "/sys/class/gpio"
#define LED    17
#define BUTTON 27

static int write_file(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int rc = (write(fd, val, strlen(val)) < 0) ? -1 : 0;
    close(fd);
    return rc;
}

static void export_pin(int n)
{
    char num[8];
    snprintf(num, sizeof(num), "%d", n);
    /* Ignore EBUSY: already exported from a previous run. */
    write_file(SYSFS "/export", num);
}

static void unexport_pin(int n)
{
    char num[8];
    snprintf(num, sizeof(num), "%d", n);
    write_file(SYSFS "/unexport", num);
}

int main(void)
{
    char path[64];

    /* --- blink the LED --- */
    export_pin(LED);
    snprintf(path, sizeof(path), SYSFS "/gpio%d/direction", LED);
    if (write_file(path, "out") < 0) { perror("led direction"); return 1; }

    char valpath[64];
    snprintf(valpath, sizeof(valpath), SYSFS "/gpio%d/value", LED);
    printf("Blinking GPIO%d...\n", LED);
    for (int i = 0; i < 6; i++) {
        write_file(valpath, (i % 2) ? "1" : "0");
        usleep(100000);
    }
    write_file(valpath, "0");

    /* --- set up the button: input, interrupt on the rising edge --- */
    export_pin(BUTTON);
    snprintf(path, sizeof(path), SYSFS "/gpio%d/direction", BUTTON);
    if (write_file(path, "in") < 0) { perror("button direction"); return 1; }
    snprintf(path, sizeof(path), SYSFS "/gpio%d/edge", BUTTON);
    if (write_file(path, "rising") < 0) { perror("button edge"); return 1; }

    snprintf(path, sizeof(path), SYSFS "/gpio%d/value", BUTTON);
    int vfd = open(path, O_RDONLY);
    if (vfd < 0) { perror("button value"); return 1; }

    /* An initial read clears any pending state (kernel-style rearm). */
    char buf[8];
    lseek(vfd, 0, SEEK_SET);
    if (read(vfd, buf, sizeof(buf)) < 0) { /* ignore */ }

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLPRI | EPOLLERR, .data.fd = vfd };
    epoll_ctl(ep, EPOLL_CTL_ADD, vfd, &ev);

    printf("WAITING for button press on GPIO%d (up to 5s)...\n", BUTTON);
    fflush(stdout);

    struct epoll_event out;
    int n = epoll_wait(ep, &out, 1, 5000);
    int rc;
    if (n > 0) {
        lseek(vfd, 0, SEEK_SET);
        ssize_t r = read(vfd, buf, sizeof(buf) - 1);
        if (r > 0) buf[r] = '\0';
        for (char *s = buf; *s; s++) if (*s == '\n') *s = '\0';
        printf("Button pressed! value=%s\n", buf);
        rc = 0;
    } else {
        printf("timeout: no button press\n");
        rc = 3;
    }

    close(vfd);
    unexport_pin(LED);
    unexport_pin(BUTTON);
    return rc;
}

/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * gpio_sysfs.c — FUSE filesystem that emulates /sys/class/gpio.
 *
 * The tree served:
 *
 *   /
 *   ├── export            (write-only)  echo N  -> creates gpioN/
 *   ├── unexport          (write-only)  echo N  -> removes  gpioN/
 *   ├── gpiochip<base>/
 *   │   ├── base label ngpio   (read-only)
 *   └── gpioN/            (exists only while exported)
 *       ├── direction     in | out | high | low
 *       ├── value         0 | 1   (pollable — see below)
 *       ├── edge          none | rising | falling | both
 *       └── active_low    0 | 1
 *
 * THE POLL LINCHPIN
 * -----------------
 * A program does poll(2)/epoll on gpioN/value with POLLPRI and blocks until the
 * input line transitions in the configured `edge` direction. We implement this
 * with libfuse's `.poll` op: when the kernel calls .poll it hands us a
 * fuse_pollhandle; we stash it against the open fd. When the model reports a
 * matching edge (via the subscriber callback, under the model lock) we call
 * fuse_notify_poll(ph), which makes the kernel re-poll; we then return
 * POLLPRI|POLLERR — exactly what the real sysfs value attribute returns.
 *
 * A per-open "has_event" latch reproduces kernel semantics: once an edge fires,
 * poll returns POLLPRI until the program clears it by seeking to 0 and reading.
 *
 * value files are opened with direct_io so every read reaches us (no page cache),
 * which is what makes the seek-and-read rearm work.
 */
#define FUSE_USE_VERSION 31

#include "gpio_sysfs.h"
#include "gpio_log.h"

#include <errno.h>
#include <fuse.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- per-open state for value files ------------------------------------- */

typedef struct value_watch {
    struct value_watch *next;     /* next open fd on the same line            */
    int                 number;   /* which gpio                               */
    struct fuse_pollhandle *ph;   /* pending poll handle to notify, or NULL   */
    int                 has_event;/* edge latched since last read?            */
} value_watch;

/* The head of each line's watch list lives in gpio_line.fe_priv. All list access
 * is done under the model lock so it can't race the subscriber callback. */
static value_watch *watch_list(gpio_line *l) { return (value_watch *)l->fe_priv; }

static void watch_push(gpio_line *l, value_watch *w)
{
    w->next    = watch_list(l);
    l->fe_priv = w;
}

static void watch_remove(gpio_model *m, value_watch *w)
{
    gpio_line *l = gpio_model_line(m, w->number);
    if (!l)
        return;
    value_watch **pp = (value_watch **)&l->fe_priv;
    while (*pp) {
        if (*pp == w) { *pp = w->next; return; }
        pp = &(*pp)->next;
    }
}

/* ---- model → sysfs subscriber ------------------------------------------- */

/* Wake every fd watching this line: latch the event and, if the fd is currently
 * blocked in poll(), notify the kernel. Runs with the model lock held. */
static void wake_watchers(gpio_line *l)
{
    for (value_watch *w = watch_list(l); w; w = w->next) {
        w->has_event = 1;
        if (w->ph) {
            fuse_notify_poll(w->ph);
            fuse_pollhandle_destroy(w->ph);
            w->ph = NULL;
        }
    }
}

static void sysfs_on_change(gpio_model *m, gpio_line *l,
                            int oldv, int newv, bool edge_match, void *ud)
{
    (void)m; (void)oldv; (void)newv; (void)ud;
    if (edge_match)
        wake_watchers(l);
}

static void sysfs_on_export(gpio_model *m, gpio_line *l, bool exported, void *ud)
{
    (void)m; (void)ud;
    /* On unexport/reboot, wake any pollers so they return (POLLERR) instead of
     * hanging on a line that no longer exists. */
    if (!exported)
        wake_watchers(l);
}

/* ---- path parsing ------------------------------------------------------- */

typedef enum {
    P_ROOT, P_EXPORT, P_UNEXPORT,
    P_CHIP, P_CHIP_ATTR,
    P_GPIO, P_GPIO_ATTR,
    P_NONE,
} path_kind;

typedef struct {
    path_kind kind;
    int       number;      /* gpio number for P_GPIO*, ignored otherwise */
    char      attr[16];    /* attribute name for *_ATTR                  */
} parsed;

/* Return true if `name` is a valid attribute of a gpioN/ directory. */
static int is_gpio_attr(const char *name)
{
    return !strcmp(name, "direction") || !strcmp(name, "value") ||
           !strcmp(name, "edge")      || !strcmp(name, "active_low");
}

static int is_chip_attr(const char *name)
{
    return !strcmp(name, "base") || !strcmp(name, "label") || !strcmp(name, "ngpio");
}

static parsed parse_path(gpio_model *m, const char *path)
{
    parsed p = { .kind = P_NONE, .number = -1, .attr = {0} };
    int base = gpio_model_base(m);

    if (!strcmp(path, "/"))          { p.kind = P_ROOT;     return p; }
    if (!strcmp(path, "/export"))    { p.kind = P_EXPORT;   return p; }
    if (!strcmp(path, "/unexport"))  { p.kind = P_UNEXPORT; return p; }

    /* /gpiochip<base>  or  /gpiochip<base>/<attr> */
    char chipdir[32];
    snprintf(chipdir, sizeof(chipdir), "/gpiochip%d", base);
    size_t chiplen = strlen(chipdir);
    if (!strncmp(path, chipdir, chiplen) &&
        (path[chiplen] == '\0' || path[chiplen] == '/')) {
        if (path[chiplen] == '\0') { p.kind = P_CHIP; return p; }
        const char *attr = path + chiplen + 1;
        if (*attr && !strchr(attr, '/') && is_chip_attr(attr)) {
            p.kind = P_CHIP_ATTR;
            snprintf(p.attr, sizeof(p.attr), "%s", attr);
            return p;
        }
        return p; /* P_NONE */
    }

    /* /gpio<N>  or  /gpio<N>/<attr> */
    int n = -1;
    char rest[32] = {0};
    /* sscanf: "%n"-free parse — require the "gpio" prefix then digits. */
    if (sscanf(path, "/gpio%d", &n) == 1 && n >= 0) {
        char prefix[32];
        int len = snprintf(prefix, sizeof(prefix), "/gpio%d", n);
        if (path[len] == '\0') {
            p.kind = P_GPIO; p.number = n; return p;
        }
        if (path[len] == '/') {
            snprintf(rest, sizeof(rest), "%s", path + len + 1);
            if (*rest && !strchr(rest, '/') && is_gpio_attr(rest)) {
                p.kind = P_GPIO_ATTR; p.number = n;
                snprintf(p.attr, sizeof(p.attr), "%s", rest);
                return p;
            }
        }
    }
    return p; /* P_NONE */
}

/* ---- rendering attribute contents --------------------------------------- */

/* Format the current textual contents of a readable file into buf. Must be called
 * with the model lock held. Returns bytes written, or -errno. */
static int render(gpio_model *m, const parsed *p, char *buf, size_t cap)
{
    gpio_line *l;
    switch (p->kind) {
    case P_CHIP_ATTR:
        if (!strcmp(p->attr, "base"))  return snprintf(buf, cap, "%d\n", gpio_model_base(m));
        if (!strcmp(p->attr, "ngpio")) return snprintf(buf, cap, "%d\n", gpio_model_ngpio(m));
        if (!strcmp(p->attr, "label")) return snprintf(buf, cap, "%s\n", gpio_model_label(m));
        return -ENOENT;
    case P_GPIO_ATTR:
        l = gpio_model_line(m, p->number);
        if (!l || !l->exported) return -ENOENT;
        if (!strcmp(p->attr, "direction"))
            return snprintf(buf, cap, "%s\n", l->direction == GPIO_DIR_OUT ? "out" : "in");
        if (!strcmp(p->attr, "value"))
            return snprintf(buf, cap, "%d\n", gpio_line_logical(l));
        if (!strcmp(p->attr, "edge"))
            return snprintf(buf, cap, "%s\n", gpio_edge_to_str(l->edge));
        if (!strcmp(p->attr, "active_low"))
            return snprintf(buf, cap, "%d\n", l->active_low ? 1 : 0);
        return -ENOENT;
    default:
        return -EACCES; /* export/unexport are write-only; dirs aren't read */
    }
}

/* ---- FUSE operations ---------------------------------------------------- */

static gpio_model *ctx_model(void)
{
    return (gpio_model *)fuse_get_context()->private_data;
}

static int gp_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void)fi;
    gpio_model *m = ctx_model();
    parsed p = parse_path(m, path);

    memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();

    switch (p.kind) {
    case P_ROOT:
    case P_CHIP:
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    case P_GPIO: {
        gpio_model_lock(m);
        gpio_line *l = gpio_model_line(m, p.number);
        int ok = l && l->exported;
        gpio_model_unlock(m);
        if (!ok) return -ENOENT;
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }
    case P_EXPORT:
    case P_UNEXPORT:
        st->st_mode = S_IFREG | 0200;   /* write-only */
        st->st_nlink = 1;
        st->st_size = 4096;
        return 0;
    case P_CHIP_ATTR:
        st->st_mode = S_IFREG | 0444;   /* read-only */
        st->st_nlink = 1;
        st->st_size = 4096;
        return 0;
    case P_GPIO_ATTR: {
        gpio_model_lock(m);
        gpio_line *l = gpio_model_line(m, p.number);
        int ok = l && l->exported;
        gpio_model_unlock(m);
        if (!ok) return -ENOENT;
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = 4096;
        return 0;
    }
    default:
        return -ENOENT;
    }
}

static int gp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;
    gpio_model *m = ctx_model();
    parsed p = parse_path(m, path);

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    if (p.kind == P_ROOT) {
        filler(buf, "export", NULL, 0, 0);
        filler(buf, "unexport", NULL, 0, 0);
        char chip[32];
        snprintf(chip, sizeof(chip), "gpiochip%d", gpio_model_base(m));
        filler(buf, chip, NULL, 0, 0);
        gpio_model_lock(m);
        int base = gpio_model_base(m), n = gpio_model_ngpio(m);
        for (int i = 0; i < n; i++) {
            gpio_line *l = gpio_model_line(m, base + i);
            if (l->exported) {
                char name[24];
                snprintf(name, sizeof(name), "gpio%d", l->number);
                filler(buf, name, NULL, 0, 0);
            }
        }
        gpio_model_unlock(m);
        return 0;
    }
    if (p.kind == P_CHIP) {
        filler(buf, "base", NULL, 0, 0);
        filler(buf, "label", NULL, 0, 0);
        filler(buf, "ngpio", NULL, 0, 0);
        return 0;
    }
    if (p.kind == P_GPIO) {
        gpio_model_lock(m);
        gpio_line *l = gpio_model_line(m, p.number);
        int ok = l && l->exported;
        gpio_model_unlock(m);
        if (!ok) return -ENOENT;
        filler(buf, "direction", NULL, 0, 0);
        filler(buf, "value", NULL, 0, 0);
        filler(buf, "edge", NULL, 0, 0);
        filler(buf, "active_low", NULL, 0, 0);
        return 0;
    }
    return -ENOENT;
}

static int gp_open(const char *path, struct fuse_file_info *fi)
{
    gpio_model *m = ctx_model();
    parsed p = parse_path(m, path);

    if (p.kind == P_NONE)
        return -ENOENT;

    /* Only the value file gets per-open poll state. */
    if (p.kind == P_GPIO_ATTR && !strcmp(p.attr, "value")) {
        gpio_model_lock(m);
        gpio_line *l = gpio_model_line(m, p.number);
        if (!l || !l->exported) { gpio_model_unlock(m); return -ENOENT; }
        value_watch *w = calloc(1, sizeof(*w));
        if (!w) { gpio_model_unlock(m); return -ENOMEM; }
        w->number = p.number;
        watch_push(l, w);
        gpio_model_unlock(m);
        fi->fh = (uint64_t)(uintptr_t)w;
        fi->direct_io = 1;   /* every read hits us -> seek+read rearm works */
        return 0;
    }

    fi->fh = 0;
    return 0;
}

static int gp_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    gpio_model *m = ctx_model();
    parsed p = parse_path(m, path);

    char tmp[128];
    gpio_model_lock(m);
    int n = render(m, &p, tmp, sizeof(tmp));
    /* Reading the value file clears the latched poll event (kernel semantics). */
    if (n >= 0 && fi->fh) {
        value_watch *w = (value_watch *)(uintptr_t)fi->fh;
        w->has_event = 0;
    }
    gpio_model_unlock(m);

    if (n < 0)
        return n;
    if (offset >= n)
        return 0;
    size_t avail = (size_t)n - (size_t)offset;
    size_t cnt = avail < size ? avail : size;
    memcpy(buf, tmp + offset, cnt);
    return (int)cnt;
}

/* Trim leading/trailing whitespace from a NUL-terminated copy of the write buf. */
static void trim(char *s)
{
    size_t len = strlen(s);
    while (len && (s[len-1] == '\n' || s[len-1] == '\r' ||
                   s[len-1] == ' '  || s[len-1] == '\t'))
        s[--len] = '\0';
}

static int gp_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    (void)offset; (void)fi;
    gpio_model *m = ctx_model();
    parsed p = parse_path(m, path);

    char val[64];
    size_t cp = size < sizeof(val) - 1 ? size : sizeof(val) - 1;
    memcpy(val, buf, cp);
    val[cp] = '\0';
    trim(val);

    int rc = 0;
    switch (p.kind) {
    case P_EXPORT:
    case P_UNEXPORT: {
        char *end;
        long num = strtol(val, &end, 10);
        if (end == val) return -EINVAL;
        rc = (p.kind == P_EXPORT) ? gpio_model_export(m, (int)num)
                                  : gpio_model_unexport(m, (int)num);
        break;
    }
    case P_GPIO_ATTR:
        if (!strcmp(p.attr, "direction")) {
            rc = gpio_model_set_direction(m, p.number, val);
        } else if (!strcmp(p.attr, "value")) {
            char *end; long v = strtol(val, &end, 0);
            if (end == val) return -EINVAL;
            rc = gpio_model_write_value(m, p.number, (int)v);
        } else if (!strcmp(p.attr, "edge")) {
            gpio_edge_t e;
            rc = gpio_edge_from_str(val, &e);
            if (rc == 0) rc = gpio_model_set_edge(m, p.number, e);
        } else if (!strcmp(p.attr, "active_low")) {
            char *end; long v = strtol(val, &end, 0);
            if (end == val) return -EINVAL;
            rc = gpio_model_set_active_low(m, p.number, v != 0);
        } else {
            rc = -EACCES;
        }
        break;
    default:
        rc = -EACCES;
    }

    if (rc < 0)
        return rc;
    return (int)size;   /* report the whole write consumed */
}

/* Libraries open value with O_WRONLY|O_TRUNC; make truncate a no-op success. */
static int gp_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)path; (void)size; (void)fi;
    return 0;
}

static int gp_poll(const char *path, struct fuse_file_info *fi,
                   struct fuse_pollhandle *ph, unsigned *reventsp)
{
    (void)path;
    gpio_model *m = ctx_model();

    if (!fi->fh) {
        /* Non-value file: report readable, discard any poll handle. */
        if (ph) fuse_pollhandle_destroy(ph);
        *reventsp = POLLIN;
        return 0;
    }

    gpio_model_lock(m);
    value_watch *w = (value_watch *)(uintptr_t)fi->fh;
    unsigned re = POLLIN;
    if (w->has_event) {
        /* Event already pending — deliverable now; no need to stash the handle. */
        re |= POLLPRI | POLLERR;
        if (ph) fuse_pollhandle_destroy(ph);
    } else if (ph) {
        if (w->ph) fuse_pollhandle_destroy(w->ph);
        w->ph = ph;   /* wake it when the next matching edge arrives */
    }
    *reventsp = re;
    gpio_model_unlock(m);
    return 0;
}

static int gp_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    if (!fi->fh)
        return 0;
    gpio_model *m = ctx_model();
    gpio_model_lock(m);
    value_watch *w = (value_watch *)(uintptr_t)fi->fh;
    watch_remove(m, w);
    if (w->ph) fuse_pollhandle_destroy(w->ph);
    gpio_model_unlock(m);
    free(w);
    return 0;
}

static const struct fuse_operations gpio_ops = {
    .getattr  = gp_getattr,
    .readdir  = gp_readdir,
    .open     = gp_open,
    .read     = gp_read,
    .write    = gp_write,
    .truncate = gp_truncate,
    .poll     = gp_poll,
    .release  = gp_release,
};

/* ---- entry point -------------------------------------------------------- */

int gpio_sysfs_run(gpio_model *model, const char *mountpoint,
                   int foreground, int debug, int allow_other)
{
    /* Register the sysfs frontend as a model subscriber (poll wakeups). */
    static gpio_subscriber sub;
    sub.on_change = sysfs_on_change;
    sub.on_export = sysfs_on_export;
    sub.ud = NULL;
    gpio_model_subscribe(model, &sub);

    /* Build a fuse argv. We keep it explicit rather than parsing user args so the
     * mount options that matter (below) are always applied. */
    char *argv[12];
    int argc = 0;
    argv[argc++] = (char *)"gpio-sim";
    argv[argc++] = (char *)mountpoint;
    if (foreground) argv[argc++] = (char *)"-f";
    if (debug)      argv[argc++] = (char *)"-d";
    /* Mount options (see ADR 0001 / gpiod-sysfs-proxy notes):
     *   default_permissions  — kernel enforces the mode bits we report
     *   allow_other          — tools run by other users (and the overlay path) reach it
     *   entry_timeout=0      — never cache dentries, so an unexport'd gpioN/ disappears
     *   attr_timeout=0       — never cache attrs, so value/direction reads are live
     *   negative_timeout=0   — never cache "does not exist", so a fresh export appears
     * The zero timeouts are essential: without them the kernel serves stale directory
     * and value data and the sim looks broken right after export/unexport. */
    if (allow_other)
        argv[argc++] = (char *)"-oallow_other,default_permissions,"
                               "entry_timeout=0,attr_timeout=0,negative_timeout=0";
    else
        argv[argc++] = (char *)"-odefault_permissions,"
                               "entry_timeout=0,attr_timeout=0,negative_timeout=0";
    argv[argc] = NULL;

    LOG_INFO("sysfs: mounting at %s (%d args)", mountpoint, argc);
    return fuse_main(argc, argv, &gpio_ops, model);
}

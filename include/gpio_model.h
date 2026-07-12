/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * gpio_model.h — the authoritative in-memory GPIO "line" model.
 *
 * This is the single source of truth for pin state (ADR 0001, decision #2). Every
 * mutation — a sysfs `echo 1 > value`, a control-API call, a panel click — goes
 * through these functions, which hold a single lock and fan every change out to
 * subscribed frontends (the sysfs FUSE layer, and later the web panel).
 *
 * The model is deliberately frontend-agnostic (ADR 0001, decision #3): the sysfs
 * tree is one subscriber onto this model, so a future chardev/i2c frontend can be
 * added as a sibling without touching the core.
 *
 * Portable C11 — no FUSE, no OS-specific headers — so it builds and unit-tests on
 * any platform (including macOS during development).
 */
#ifndef GPIO_MODEL_H
#define GPIO_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { GPIO_DIR_IN = 0, GPIO_DIR_OUT = 1 } gpio_dir_t;

typedef enum {
    GPIO_EDGE_NONE = 0,
    GPIO_EDGE_RISING,
    GPIO_EDGE_FALLING,
    GPIO_EDGE_BOTH,
} gpio_edge_t;

/*
 * A single GPIO line.
 *
 * `phys` is the PHYSICAL line level (0/1) — what the panel LED shows. The value a
 * program reads/writes via sysfs is the LOGICAL level, which is `phys ^ active_low`
 * (see gpio_line_logical). Storing the physical level keeps the panel and the
 * active_low inversion semantics consistent from one place.
 */
typedef struct gpio_line {
    int         number;      /* global GPIO number (chip base + offset)          */
    bool        exported;    /* has userspace exported this line?                */
    gpio_dir_t  direction;
    int         phys;        /* physical line level, 0 or 1                      */
    bool        active_low;  /* invert logical value for reads and writes        */
    gpio_edge_t edge;        /* which input transitions wake poll()              */
    uint64_t    generation;  /* bumped on every logical-value change             */
    void       *fe_priv;     /* frontend-owned (e.g. list of poll watchers)      */
} gpio_line;

typedef struct gpio_model gpio_model;

/*
 * A frontend that wants to observe the model. Both callbacks are invoked with the
 * model lock HELD, so they must be quick and must NOT call back into any locking
 * gpio_model_* function (the lock is non-recursive). Touching your own per-line
 * state stored in gpio_line.fe_priv is fine and expected.
 */
typedef struct gpio_subscriber {
    /* A line's logical value changed. `edge_match` is true only for INPUT lines
     * whose configured edge matches this transition — that is the signal a
     * poll()/epoll() waiter is waiting for. */
    void (*on_change)(gpio_model *m, gpio_line *l,
                      int old_logical, int new_logical,
                      bool edge_match, void *ud);
    /* A line was exported (exported=true) or unexported/rebooted (exported=false).
     * Frontends use this to drop poll watchers and refresh the panel. */
    void (*on_export)(gpio_model *m, gpio_line *l, bool exported, void *ud);
    void *ud;
    struct gpio_subscriber *next; /* internal; set by gpio_model_subscribe */
} gpio_subscriber;

/* ---- lifecycle ---------------------------------------------------------- */

gpio_model *gpio_model_create(int base, int ngpio, const char *label);
void        gpio_model_destroy(gpio_model *m);

/* ---- chip attributes ---------------------------------------------------- */

int         gpio_model_base(const gpio_model *m);
int         gpio_model_ngpio(const gpio_model *m);
const char *gpio_model_label(const gpio_model *m);

/* Register a subscriber. Call during setup, before frontends start running. */
void        gpio_model_subscribe(gpio_model *m, gpio_subscriber *sub);

/* ---- locking ------------------------------------------------------------
 * Frontends protect their own per-line state (fe_priv) with the SAME lock so
 * that subscriber callbacks (invoked under lock) and frontend I/O paths never
 * race. Never call a locking gpio_model_* function while already holding it. */
void        gpio_model_lock(gpio_model *m);
void        gpio_model_unlock(gpio_model *m);

/* Look up a line by number. MUST be called with the lock held. Returns NULL if
 * `number` is outside [base, base+ngpio). */
gpio_line  *gpio_model_line(gpio_model *m, int number);

/* ---- sysfs operations (each takes the lock internally) ------------------
 * All return 0 on success or a negative errno matching kernel sysfs behaviour. */

int gpio_model_export(gpio_model *m, int number);        /* -EINVAL / -EBUSY */
int gpio_model_unexport(gpio_model *m, int number);      /* -EINVAL          */
int gpio_model_set_direction(gpio_model *m, int number,
                             const char *dir);            /* "in|out|high|low" */
int gpio_model_read_value(gpio_model *m, int number);    /* 0/1 or -errno    */
int gpio_model_write_value(gpio_model *m, int number,
                           int value);                    /* outputs only, else -EPERM */
int gpio_model_set_active_low(gpio_model *m, int number, bool active_low);
int gpio_model_set_edge(gpio_model *m, int number, gpio_edge_t edge);

/* ---- panel / control-API ------------------------------------------------ */

/* Drive an exported INPUT line's physical level (this is the "line" the panel and
 * control API own). Fires edges → wakes matching poll() waiters. Returns 0, or
 * -ENOENT (not exported) / -EPERM (line is an output). */
int gpio_model_drive_line(gpio_model *m, int number, int level);

/* Reset every line to power-on defaults: unexported, input, low, active_low=0,
 * edge=none. Mirrors a board reboot. */
void gpio_model_reboot(gpio_model *m);

/* ---- pure helpers ------------------------------------------------------- */

int         gpio_edge_from_str(const char *s, gpio_edge_t *out); /* 0 or -EINVAL */
const char *gpio_edge_to_str(gpio_edge_t e);

/* The logical value a program sees: physical level inverted by active_low. */
static inline int gpio_line_logical(const gpio_line *l)
{
    return l->phys ^ (l->active_low ? 1 : 0);
}

#ifdef __cplusplus
}
#endif
#endif /* GPIO_MODEL_H */

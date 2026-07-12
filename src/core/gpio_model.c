/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "gpio_model.h"
#include "gpio_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gpio_model {
    pthread_mutex_t  lock;
    int              base;
    int              ngpio;
    char             label[64];
    gpio_line       *lines;   /* array of ngpio lines                     */
    gpio_subscriber *subs;    /* singly-linked list, head-inserted        */
};

/* ---- internals (assume lock held) --------------------------------------- */

static gpio_line *line_at(gpio_model *m, int number)
{
    int idx = number - m->base;
    if (idx < 0 || idx >= m->ngpio)
        return NULL;
    return &m->lines[idx];
}

static void reset_line(gpio_line *l, int number)
{
    l->number     = number;
    l->exported   = false;
    l->direction  = GPIO_DIR_IN;
    l->phys       = 0;
    l->active_low = false;
    l->edge       = GPIO_EDGE_NONE;
    l->generation = 0;
    /* fe_priv is owned by the frontend; never touched here. */
}

/* Does a logical old->new transition match this line's edge config? Only inputs
 * generate interrupts, so outputs never "match" (mirrors kernel behaviour). */
static bool edge_matches(const gpio_line *l, int oldv, int newv)
{
    if (l->direction != GPIO_DIR_IN || oldv == newv)
        return false;
    switch (l->edge) {
    case GPIO_EDGE_RISING:  return oldv == 0 && newv == 1;
    case GPIO_EDGE_FALLING: return oldv == 1 && newv == 0;
    case GPIO_EDGE_BOTH:    return true;
    case GPIO_EDGE_NONE:
    default:                return false;
    }
}

static void emit_change(gpio_model *m, gpio_line *l,
                        int oldv, int newv, bool edge_match)
{
    for (gpio_subscriber *s = m->subs; s; s = s->next)
        if (s->on_change)
            s->on_change(m, l, oldv, newv, edge_match, s->ud);
}

static void emit_export(gpio_model *m, gpio_line *l, bool exported)
{
    for (gpio_subscriber *s = m->subs; s; s = s->next)
        if (s->on_export)
            s->on_export(m, l, exported, s->ud);
}

/* Set the physical level and, if the logical value changed, bump the generation
 * and notify subscribers. Returns whether a matching edge fired. */
static bool apply_phys(gpio_model *m, gpio_line *l, int phys)
{
    phys = phys ? 1 : 0;
    int oldv = gpio_line_logical(l);
    l->phys  = phys;
    int newv = gpio_line_logical(l);
    if (oldv == newv)
        return false;
    l->generation++;
    bool match = edge_matches(l, oldv, newv);
    emit_change(m, l, oldv, newv, match);
    return match;
}

/* ---- lifecycle ---------------------------------------------------------- */

gpio_model *gpio_model_create(int base, int ngpio, const char *label)
{
    if (ngpio <= 0 || base < 0)
        return NULL;

    gpio_model *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;

    m->lines = calloc((size_t)ngpio, sizeof(gpio_line));
    if (!m->lines) {
        free(m);
        return NULL;
    }

    pthread_mutex_init(&m->lock, NULL);
    m->base  = base;
    m->ngpio = ngpio;
    snprintf(m->label, sizeof(m->label), "%s", label ? label : "gpio-sim");
    for (int i = 0; i < ngpio; i++)
        reset_line(&m->lines[i], base + i);

    LOG_INFO("model: chip '%s' base=%d ngpio=%d (gpio%d..gpio%d)",
             m->label, base, ngpio, base, base + ngpio - 1);
    return m;
}

void gpio_model_destroy(gpio_model *m)
{
    if (!m)
        return;
    pthread_mutex_destroy(&m->lock);
    free(m->lines);
    free(m);
}

int         gpio_model_base(const gpio_model *m)  { return m->base; }
int         gpio_model_ngpio(const gpio_model *m) { return m->ngpio; }
const char *gpio_model_label(const gpio_model *m) { return m->label; }

void gpio_model_subscribe(gpio_model *m, gpio_subscriber *sub)
{
    sub->next = m->subs;
    m->subs   = sub;
}

void gpio_model_lock(gpio_model *m)   { pthread_mutex_lock(&m->lock); }
void gpio_model_unlock(gpio_model *m) { pthread_mutex_unlock(&m->lock); }

gpio_line *gpio_model_line(gpio_model *m, int number) { return line_at(m, number); }

/* ---- sysfs operations --------------------------------------------------- */

int gpio_model_export(gpio_model *m, int number)
{
    pthread_mutex_lock(&m->lock);
    gpio_line *l = line_at(m, number);
    int rc = 0;
    if (!l) {
        rc = -EINVAL;                 /* out of range */
    } else if (l->exported) {
        rc = -EBUSY;                  /* already exported */
    } else {
        /* Fresh export: input, edge none, active_low 0 (kernel defaults).
         * The physical level is left as-is so a panel-set input level survives. */
        l->exported   = true;
        l->direction  = GPIO_DIR_IN;
        l->edge       = GPIO_EDGE_NONE;
        l->active_low = false;
        emit_export(m, l, true);
        LOG_DBG("export gpio%d", number);
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int gpio_model_unexport(gpio_model *m, int number)
{
    pthread_mutex_lock(&m->lock);
    gpio_line *l = line_at(m, number);
    int rc = 0;
    if (!l || !l->exported) {
        rc = -EINVAL;                 /* not exported / out of range */
    } else {
        l->exported = false;
        l->edge     = GPIO_EDGE_NONE; /* drop transient interrupt config */
        emit_export(m, l, false);     /* frontend wakes+drops poll watchers */
        LOG_DBG("unexport gpio%d", number);
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int gpio_model_set_direction(gpio_model *m, int number, const char *dir)
{
    pthread_mutex_lock(&m->lock);
    gpio_line *l = line_at(m, number);
    int rc = 0;
    if (!l || !l->exported) {
        rc = -ENOENT;
    } else if (strcmp(dir, "in") == 0) {
        l->direction = GPIO_DIR_IN;
    } else if (strcmp(dir, "out") == 0 || strcmp(dir, "low") == 0) {
        /* "out" defaults to logical low; "low" is the explicit glitch-free form. */
        l->direction = GPIO_DIR_OUT;
        apply_phys(m, l, l->active_low ? 1 : 0);   /* logical 0 */
    } else if (strcmp(dir, "high") == 0) {
        /* glitch-free: become an output already driving logical high */
        l->direction = GPIO_DIR_OUT;
        apply_phys(m, l, l->active_low ? 0 : 1);   /* logical 1 */
    } else {
        rc = -EINVAL;
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int gpio_model_read_value(gpio_model *m, int number)
{
    pthread_mutex_lock(&m->lock);
    gpio_line *l = line_at(m, number);
    int rc = (!l || !l->exported) ? -ENOENT : gpio_line_logical(l);
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int gpio_model_write_value(gpio_model *m, int number, int value)
{
    pthread_mutex_lock(&m->lock);
    gpio_line *l = line_at(m, number);
    int rc = 0;
    if (!l || !l->exported) {
        rc = -ENOENT;
    } else if (l->direction != GPIO_DIR_OUT) {
        rc = -EPERM;                  /* kernel: writing value on an input => EPERM */
    } else {
        int logical = value ? 1 : 0;  /* any nonzero => high */
        apply_phys(m, l, logical ^ (l->active_low ? 1 : 0));
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int gpio_model_set_active_low(gpio_model *m, int number, bool active_low)
{
    pthread_mutex_lock(&m->lock);
    gpio_line *l = line_at(m, number);
    int rc = 0;
    if (!l || !l->exported) {
        rc = -ENOENT;
    } else if (l->active_low != active_low) {
        /* Polarity flip inverts the logical value for reads and writes while the
         * physical line is unchanged. Notify subscribers (so the web layer can
         * refresh) but never as an interrupt edge. */
        int oldv = gpio_line_logical(l);
        l->active_low = active_low;
        int newv = gpio_line_logical(l);
        if (oldv != newv) {
            l->generation++;
            emit_change(m, l, oldv, newv, false);
        }
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

int gpio_model_set_edge(gpio_model *m, int number, gpio_edge_t edge)
{
    pthread_mutex_lock(&m->lock);
    gpio_line *l = line_at(m, number);
    int rc = (!l || !l->exported) ? -ENOENT : 0;
    if (rc == 0)
        l->edge = edge;
    pthread_mutex_unlock(&m->lock);
    return rc;
}

/* ---- panel / control-API ------------------------------------------------ */

int gpio_model_drive_line(gpio_model *m, int number, int level)
{
    pthread_mutex_lock(&m->lock);
    gpio_line *l = line_at(m, number);
    int rc = 0;
    if (!l || !l->exported) {
        rc = -ENOENT;
    } else if (l->direction != GPIO_DIR_IN) {
        /* Outputs are driven by code, not the panel (ADR 0001, §7). */
        rc = -EPERM;
    } else {
        apply_phys(m, l, level ? 1 : 0);
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

void gpio_model_reboot(gpio_model *m)
{
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < m->ngpio; i++) {
        gpio_line *l = &m->lines[i];
        bool was_exported = l->exported;
        int  oldv = gpio_line_logical(l);
        reset_line(l, l->number);
        if (was_exported)
            emit_export(m, l, false);          /* drop poll watchers */
        int newv = gpio_line_logical(l);
        if (oldv != newv)
            emit_change(m, l, oldv, newv, false);
    }
    LOG_INFO("model: reboot — all lines reset to power-on defaults");
    pthread_mutex_unlock(&m->lock);
}

/* ---- pure helpers ------------------------------------------------------- */

int gpio_edge_from_str(const char *s, gpio_edge_t *out)
{
    if (strcmp(s, "none") == 0)         *out = GPIO_EDGE_NONE;
    else if (strcmp(s, "rising") == 0)  *out = GPIO_EDGE_RISING;
    else if (strcmp(s, "falling") == 0) *out = GPIO_EDGE_FALLING;
    else if (strcmp(s, "both") == 0)    *out = GPIO_EDGE_BOTH;
    else return -EINVAL;
    return 0;
}

const char *gpio_edge_to_str(gpio_edge_t e)
{
    switch (e) {
    case GPIO_EDGE_RISING:  return "rising";
    case GPIO_EDGE_FALLING: return "falling";
    case GPIO_EDGE_BOTH:    return "both";
    case GPIO_EDGE_NONE:
    default:                return "none";
    }
}

/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test_gpio_model.c — native unit tests for the core line model.
 *
 * These run anywhere (no FUSE, no Linux needed) and cover the acceptance criteria
 * that live at the model level: export/unexport errors, direction incl. high/low,
 * value read-back, active_low inversion, write-on-input failure, the edge-match
 * decision that drives poll() wakeups, and reboot. The FUSE `.poll` delivery on
 * top of this is exercised separately by the Docker integration test.
 */
#include "gpio_model.h"
#include "gpio_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...) do {                                   \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_failures++;                                       \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
        }                                                      \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b), "expected %ld, got %ld", (long)(b), (long)(a))

/* ---- a test subscriber that records the last edge notification ---------- */

struct spy {
    int  changes;
    int  edge_matches;
    int  last_old, last_new;
    int  exports, unexports;
};

static void spy_on_change(gpio_model *m, gpio_line *l,
                          int oldv, int newv, bool edge_match, void *ud)
{
    (void)m; (void)l;
    struct spy *s = ud;
    s->changes++;
    s->last_old = oldv;
    s->last_new = newv;
    if (edge_match)
        s->edge_matches++;
}

static void spy_on_export(gpio_model *m, gpio_line *l, bool exported, void *ud)
{
    (void)m; (void)l;
    struct spy *s = ud;
    if (exported) s->exports++;
    else          s->unexports++;
}

static gpio_model *fresh(struct spy *spy, gpio_subscriber *sub)
{
    gpio_model *m = gpio_model_create(0, 28, "pinctrl-test");
    memset(spy, 0, sizeof(*spy));
    memset(sub, 0, sizeof(*sub));
    sub->on_change = spy_on_change;
    sub->on_export = spy_on_export;
    sub->ud = spy;
    gpio_model_subscribe(m, sub);
    return m;
}

/* ---- helper: read the direction back by inspecting the line ------------- */

static gpio_dir_t dir_of(gpio_model *m, int n)
{
    gpio_model_lock(m);
    gpio_dir_t d = gpio_model_line(m, n)->direction;
    gpio_model_unlock(m);
    return d;
}

/* ---- tests -------------------------------------------------------------- */

static void test_export_unexport(void)
{
    printf("test_export_unexport\n");
    struct spy spy; gpio_subscriber sub;
    gpio_model *m = fresh(&spy, &sub);

    CHECK_EQ(gpio_model_export(m, 17), 0);
    CHECK_EQ(spy.exports, 1);
    /* duplicate export -> EBUSY */
    CHECK_EQ(gpio_model_export(m, 17), -EBUSY);
    /* out of range -> EINVAL */
    CHECK_EQ(gpio_model_export(m, 999), -EINVAL);
    CHECK_EQ(gpio_model_export(m, -1), -EINVAL);

    /* value/direction usable once exported */
    CHECK_EQ(gpio_model_read_value(m, 17), 0);

    CHECK_EQ(gpio_model_unexport(m, 17), 0);
    CHECK_EQ(spy.unexports, 1);
    /* double unexport / never-exported -> EINVAL */
    CHECK_EQ(gpio_model_unexport(m, 17), -EINVAL);
    CHECK_EQ(gpio_model_unexport(m, 5), -EINVAL);
    /* after unexport, files are gone -> ENOENT */
    CHECK_EQ(gpio_model_read_value(m, 17), -ENOENT);

    gpio_model_destroy(m);
}

static void test_direction(void)
{
    printf("test_direction\n");
    struct spy spy; gpio_subscriber sub;
    gpio_model *m = fresh(&spy, &sub);
    gpio_model_export(m, 4);

    /* default is input */
    CHECK_EQ(dir_of(m, 4), GPIO_DIR_IN);

    /* "high" => output driving logical 1, glitch-free */
    CHECK_EQ(gpio_model_set_direction(m, 4, "high"), 0);
    CHECK_EQ(dir_of(m, 4), GPIO_DIR_OUT);
    CHECK_EQ(gpio_model_read_value(m, 4), 1);

    /* "low" => output driving logical 0 */
    CHECK_EQ(gpio_model_set_direction(m, 4, "low"), 0);
    CHECK_EQ(gpio_model_read_value(m, 4), 0);

    /* "out" => output, defaults low */
    CHECK_EQ(gpio_model_set_direction(m, 4, "high"), 0);
    CHECK_EQ(gpio_model_set_direction(m, 4, "out"), 0);
    CHECK_EQ(gpio_model_read_value(m, 4), 0);

    /* back to input */
    CHECK_EQ(gpio_model_set_direction(m, 4, "in"), 0);
    CHECK_EQ(dir_of(m, 4), GPIO_DIR_IN);

    /* garbage => EINVAL */
    CHECK_EQ(gpio_model_set_direction(m, 4, "sideways"), -EINVAL);
    /* unexported => ENOENT */
    CHECK_EQ(gpio_model_set_direction(m, 9, "out"), -ENOENT);

    gpio_model_destroy(m);
}

static void test_value_and_active_low(void)
{
    printf("test_value_and_active_low\n");
    struct spy spy; gpio_subscriber sub;
    gpio_model *m = fresh(&spy, &sub);
    gpio_model_export(m, 22);
    gpio_model_set_direction(m, 22, "out");

    /* output value writes read back; any nonzero => 1 */
    CHECK_EQ(gpio_model_write_value(m, 22, 1), 0);
    CHECK_EQ(gpio_model_read_value(m, 22), 1);
    CHECK_EQ(gpio_model_write_value(m, 22, 0), 0);
    CHECK_EQ(gpio_model_read_value(m, 22), 0);
    CHECK_EQ(gpio_model_write_value(m, 22, 42), 0);
    CHECK_EQ(gpio_model_read_value(m, 22), 1);

    /* active_low inverts the logical value (physical line unchanged) */
    CHECK_EQ(gpio_model_set_active_low(m, 22, true), 0);
    CHECK_EQ(gpio_model_read_value(m, 22), 0);   /* was 1, now inverted */
    /* writing 1 now drives the physical line low */
    CHECK_EQ(gpio_model_write_value(m, 22, 1), 0);
    CHECK_EQ(gpio_model_read_value(m, 22), 1);   /* logical 1 ... */
    gpio_model_lock(m);
    CHECK_EQ(gpio_model_line(m, 22)->phys, 0);   /* ... but physical line is low */
    gpio_model_unlock(m);
    CHECK_EQ(gpio_model_set_active_low(m, 22, false), 0);
    CHECK_EQ(gpio_model_read_value(m, 22), 0);   /* un-invert */

    gpio_model_destroy(m);
}

static void test_write_input_fails(void)
{
    printf("test_write_input_fails\n");
    struct spy spy; gpio_subscriber sub;
    gpio_model *m = fresh(&spy, &sub);
    gpio_model_export(m, 27);
    gpio_model_set_direction(m, 27, "in");

    /* writing value to an input fails exactly like the kernel: -EPERM */
    CHECK_EQ(gpio_model_write_value(m, 27, 1), -EPERM);

    gpio_model_destroy(m);
}

static void test_edge_decision(void)
{
    printf("test_edge_decision\n");
    struct spy spy; gpio_subscriber sub;
    gpio_model *m = fresh(&spy, &sub);
    gpio_model_export(m, 23);
    gpio_model_set_direction(m, 23, "in");

    /* edge=rising: 0->1 fires, 1->0 does NOT */
    CHECK_EQ(gpio_model_set_edge(m, 23, GPIO_EDGE_RISING), 0);
    spy.edge_matches = 0;
    CHECK_EQ(gpio_model_drive_line(m, 23, 1), 0);   /* rising */
    CHECK_EQ(spy.edge_matches, 1);
    CHECK_EQ(gpio_model_drive_line(m, 23, 0), 0);   /* falling: wrong edge */
    CHECK_EQ(spy.edge_matches, 1);                   /* unchanged */

    /* edge=falling: 1->0 fires, 0->1 does NOT */
    CHECK_EQ(gpio_model_set_edge(m, 23, GPIO_EDGE_FALLING), 0);
    spy.edge_matches = 0;
    gpio_model_drive_line(m, 23, 1);                 /* rising: wrong edge */
    CHECK_EQ(spy.edge_matches, 0);
    gpio_model_drive_line(m, 23, 0);                 /* falling */
    CHECK_EQ(spy.edge_matches, 1);

    /* edge=both: any change fires */
    CHECK_EQ(gpio_model_set_edge(m, 23, GPIO_EDGE_BOTH), 0);
    spy.edge_matches = 0;
    gpio_model_drive_line(m, 23, 1);
    gpio_model_drive_line(m, 23, 0);
    CHECK_EQ(spy.edge_matches, 2);

    /* edge=none: nothing fires */
    CHECK_EQ(gpio_model_set_edge(m, 23, GPIO_EDGE_NONE), 0);
    spy.edge_matches = 0;
    gpio_model_drive_line(m, 23, 1);
    gpio_model_drive_line(m, 23, 0);
    CHECK_EQ(spy.edge_matches, 0);

    /* active_low inverts the sense of the edge: with active_low, a physical
     * rising line is a logical falling edge. */
    gpio_model_drive_line(m, 23, 0);
    gpio_model_set_active_low(m, 23, true);
    gpio_model_set_edge(m, 23, GPIO_EDGE_RISING);
    spy.edge_matches = 0;
    gpio_model_drive_line(m, 23, 1);   /* phys 0->1 == logical 1->0 == falling */
    CHECK_EQ(spy.edge_matches, 0);
    gpio_model_drive_line(m, 23, 0);   /* phys 1->0 == logical 0->1 == rising */
    CHECK_EQ(spy.edge_matches, 1);

    gpio_model_destroy(m);
}

static void test_drive_line_guards(void)
{
    printf("test_drive_line_guards\n");
    struct spy spy; gpio_subscriber sub;
    gpio_model *m = fresh(&spy, &sub);

    /* not exported */
    CHECK_EQ(gpio_model_drive_line(m, 10, 1), -ENOENT);
    /* exported output can't be driven by the panel */
    gpio_model_export(m, 10);
    gpio_model_set_direction(m, 10, "out");
    CHECK_EQ(gpio_model_drive_line(m, 10, 1), -EPERM);
    /* input can */
    gpio_model_set_direction(m, 10, "in");
    CHECK_EQ(gpio_model_drive_line(m, 10, 1), 0);
    CHECK_EQ(gpio_model_read_value(m, 10), 1);

    gpio_model_destroy(m);
}

static void test_reboot(void)
{
    printf("test_reboot\n");
    struct spy spy; gpio_subscriber sub;
    gpio_model *m = fresh(&spy, &sub);
    gpio_model_export(m, 5);
    gpio_model_set_direction(m, 5, "high");
    CHECK_EQ(gpio_model_read_value(m, 5), 1);

    gpio_model_reboot(m);
    /* everything back to power-on defaults: unexported */
    CHECK_EQ(gpio_model_read_value(m, 5), -ENOENT);
    CHECK(spy.unexports >= 1, "reboot should unexport previously-exported lines");

    /* re-export shows defaults */
    gpio_model_export(m, 5);
    CHECK_EQ(dir_of(m, 5), GPIO_DIR_IN);
    CHECK_EQ(gpio_model_read_value(m, 5), 0);

    gpio_model_destroy(m);
}

int main(void)
{
    gpio_log_level = GPIO_LOG_WARN;   /* keep test output focused */

    test_export_unexport();
    test_direction();
    test_value_and_active_low();
    test_write_input_fails();
    test_edge_decision();
    test_drive_line_guards();
    test_reboot();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

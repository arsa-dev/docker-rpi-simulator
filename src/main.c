/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * main.c — the gpio-sim daemon entry point.
 *
 * Owns one authoritative model and runs two frontends against it (ADR 0001):
 *   - the sysfs FUSE filesystem (mounted at $GPIO_MOUNT), and
 *   - the control socket ($GPIO_CONTROL_SOCK) used by tests/scripts (and, from
 *     Milestone 4, the web panel).
 *
 * Milestone 2 scope: no HTTP/web layer yet. The container entrypoint and the
 * overlay-over-/sys/class mount strategy arrive in Milestone 3; for now the mount
 * point is a plain directory (default /gpio), which is enough to run and demo the
 * sysfs core locally.
 *
 * Configuration (all via environment):
 *   GPIO_MOUNT         mount point               (default /gpio; argv[1] overrides)
 *   GPIO_CONTROL_SOCK  control socket path       (default /run/gpio-sim.sock)
 *   GPIO_BOARD         board preset: pi4|pi5|classic  (default classic)
 *   GPIO_BASE          chip base gpio number     (default from preset)
 *   GPIO_NGPIO         number of lines           (default from preset)
 *   GPIO_LABEL         chip label                (default from preset)
 *   GPIO_LOG_LEVEL     error|warn|info|debug     (default info)
 *   GPIO_ALLOW_OTHER   1/0 add -o allow_other    (default 1)
 *   GPIO_FUSE_DEBUG    1/0 libfuse -d tracing     (default 0)
 *
 * GPIO_BASE/NGPIO/LABEL, when set, override the board preset. All presets keep
 * base=0 so that sysfs gpioN == BCM N — the numbering every Pi tutorial assumes and
 * the panel's header labels line up with (ADR 0001, §6).
 */
#include "gpio_model.h"
#include "gpio_log.h"
#include "gpio_sysfs.h"
#include "control_sock.h"
#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *env_or(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    return (v && *v) ? v : dflt;
}

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    return (int)strtol(v, NULL, 10);
}

/*
 * Board presets. All keep base=0 (sysfs gpioN == BCM N) and cover BCM 0..27 — the
 * physical 40-pin header is identical across these boards; only the controller
 * label (which some tools read) and the panel's cosmetic model differ. Explicit
 * GPIO_LABEL/GPIO_BASE/GPIO_NGPIO override these.
 */
struct board_preset {
    const char *name;   /* GPIO_BOARD value            */
    const char *label;  /* default chip label          */
    int         base;
    int         ngpio;
};

static const struct board_preset k_presets[] = {
    { "classic", "pinctrl-bcm2835", 0, 28 },  /* Pi 1 / Zero / 2 / 3 (default) */
    { "pi4",     "pinctrl-bcm2711", 0, 28 },  /* Raspberry Pi 4 (BCM2711)      */
    { "pi5",     "pinctrl-rp1",     0, 28 },  /* Raspberry Pi 5 (RP1)          */
};

static const struct board_preset *resolve_preset(const char *board)
{
    for (size_t i = 0; i < sizeof(k_presets) / sizeof(k_presets[0]); i++)
        if (strcmp(board, k_presets[i].name) == 0)
            return &k_presets[i];
    return &k_presets[0];   /* unknown -> classic */
}

int main(int argc, char **argv)
{
    const char *mount    = (argc > 1) ? argv[1] : env_or("GPIO_MOUNT", "/gpio");
    const char *ctrlpath = env_or("GPIO_CONTROL_SOCK", "/run/gpio-sim.sock");

    const char *board = env_or("GPIO_BOARD", "classic");
    const struct board_preset *preset = resolve_preset(board);

    /* Preset supplies defaults; explicit env vars win. */
    int   base   = env_int("GPIO_BASE",  preset->base);
    int   ngpio  = env_int("GPIO_NGPIO", preset->ngpio);
    const char *label = env_or("GPIO_LABEL", preset->label);
    int allow_other = env_int("GPIO_ALLOW_OTHER", 1);
    int fuse_debug  = env_int("GPIO_FUSE_DEBUG", 0);
    int http_port   = env_int("GPIO_HTTP_PORT", 8080);
    const char *webroot = env_or("GPIO_WEB_ROOT", "web");

    gpio_log_level = gpio_log_level_from_str(getenv("GPIO_LOG_LEVEL"));

    /* Best-effort: ensure the (plain) mount point exists. In Milestone 3 the
     * entrypoint handles this and the overlay-over-/sys/class case. */
    mkdir(mount, 0755);

    gpio_model *model = gpio_model_create(base, ngpio, label);
    if (!model) {
        LOG_ERR("failed to create model (base=%d ngpio=%d)", base, ngpio);
        return 1;
    }

    control_sock *cs = control_sock_start(model, ctrlpath);
    if (!cs) {
        LOG_WARN("control socket unavailable — continuing without it");
    }

    web_server *web = web_server_start(model, http_port, webroot, board);
    if (!web) {
        LOG_WARN("web panel unavailable on port %d — continuing without it", http_port);
    }

    printf("\n");
    printf("  ┌─────────────────────────────────────────────────────────┐\n");
    printf("  │  gpio-sim  ·  userspace sysfs GPIO simulator              │\n");
    printf("  ├─────────────────────────────────────────────────────────┤\n");
    printf("  │  sysfs mount : %-40s │\n", mount);
    printf("  │  board       : %-40s │\n", preset->name);
    printf("  │  chip        : %-40s │\n", label);
    printf("  │  lines       : gpio%d .. gpio%-27d │\n", base, base + ngpio - 1);
    printf("  │  control     : %-40s │\n", ctrlpath);
    if (web) {
    printf("  ├─────────────────────────────────────────────────────────┤\n");
    printf("  │  ▶ web panel : http://localhost:%-24d │\n", http_port);
    }
    printf("  └─────────────────────────────────────────────────────────┘\n\n");
    fflush(stdout);

    /* Blocks until unmounted or signalled (libfuse handles SIGINT/SIGTERM). */
    int rc = gpio_sysfs_run(model, mount, /*foreground=*/1, fuse_debug, allow_other);

    web_server_stop(web);
    control_sock_stop(cs);
    gpio_model_destroy(model);
    return rc;
}

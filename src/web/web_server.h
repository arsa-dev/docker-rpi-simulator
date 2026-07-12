/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * web_server.h — the web panel + control-API frontend.
 *
 * A second subscriber onto the same model (ADR 0001, decision #2): it serves the
 * 40-pin panel, a JSON control API, and a WebSocket channel that broadcasts state
 * on every change — from any source (CLI, library, control socket, or a panel
 * click). A panel toggle of an input calls gpio_model_drive_line(), which fires the
 * same edge → the sysfs frontend's fuse_notify_poll() wakes a blocked poll() waiter.
 *
 * Implemented from scratch in C (no external HTTP/WS library) to stay AGPL-clean and
 * dependency-free; the panel's needs are modest enough that this is simpler than
 * vendoring a library and fighting license compatibility.
 */
#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "gpio_model.h"

typedef struct web_server web_server;

/*
 * Start the HTTP/WebSocket server on `port`, serving static assets from `webroot`
 * and reporting `board` in the state snapshot. Spawns its own threads and returns
 * immediately (NULL on failure). `board`/`webroot` are copied.
 */
web_server *web_server_start(gpio_model *model, int port,
                            const char *webroot, const char *board);

void        web_server_stop(web_server *ws);

#endif /* WEB_SERVER_H */

/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * control_sock.h — a tiny UNIX-socket control channel onto the model.
 *
 * This is the raw substrate of the control API (ADR 0001, §7). The web panel in
 * Milestone 4 will drive the model the same way; for now it lets tests and scripts
 * set input levels, read state, and reboot without a browser — and is what the
 * Docker integration test uses to fire edges at a blocked poller.
 *
 * Line protocol (one command per line, newline-terminated):
 *
 *   drive N V         set input line N physical level to V (0/1)   -> ok | err E
 *   read N            read logical value of line N                 -> ok V | err E
 *   export N          export line N                                -> ok | err E
 *   unexport N        unexport line N                              -> ok | err E
 *   direction N D     set direction (in|out|high|low)              -> ok | err E
 *   edge N E          set edge (none|rising|falling|both)          -> ok | err E
 *   value N V         write output value V                         -> ok | err E
 *   active_low N A    set active_low (0/1)                         -> ok | err E
 *   reboot            reset all lines to power-on defaults         -> ok
 *   ping              liveness check                               -> ok
 */
#ifndef CONTROL_SOCK_H
#define CONTROL_SOCK_H

#include "gpio_model.h"

typedef struct control_sock control_sock;

/* Bind `path`, start an accept thread, and return a handle (NULL on failure). */
control_sock *control_sock_start(gpio_model *model, const char *path);

/* Stop the accept thread, close the socket, and unlink the path. */
void          control_sock_stop(control_sock *cs);

#endif /* CONTROL_SOCK_H */

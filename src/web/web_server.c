/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * web_server.c — minimal HTTP/1.1 + WebSocket server for the panel & control API.
 *
 * Design (ADR 0001):
 *   - A model subscriber flips a `dirty` flag on any change and signals a dedicated
 *     broadcaster thread. The broadcaster snapshots state (model lock), then sends a
 *     JSON frame to every WebSocket client (clients lock). The two locks are never
 *     nested, so there is no deadlock with the accept path or /api/state.
 *   - WebSocket clients are write-only from our side: only the broadcaster writes
 *     frames, so there are no interleaved-frame races. A dead client is reaped when a
 *     send fails.
 *
 * No external HTTP/WS dependency: the handshake needs SHA-1 + base64, both inlined.
 */
#include "web_server.h"
#include "gpio_log.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* ======================= SHA-1 (public-domain style) ===================== */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a=c->h[0], b=c->h[1], d=c->h[2], e=c->h[3], f=c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t k, t;
        if (i < 20)      { t = (b & d) | (~b & e);            k = 0x5A827999; }
        else if (i < 40) { t = b ^ d ^ e;                     k = 0x6ED9EBA1; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);   k = 0x8F1BBCDC; }
        else             { t = b ^ d ^ e;                     k = 0xCA62C1D6; }
        uint32_t tmp = rol(a,5) + t + f + k + w[i];
        f = e; e = d; d = rol(b,30); b = a; a = tmp;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=d; c->h[3]+=e; c->h[4]+=f;
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0]=0x67452301; c->h[1]=0xEFCDAB89; c->h[2]=0x98BADCFE;
    c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0; c->len=0; c->n=0;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len--) {
        c->buf[c->n++] = *p++;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->n != 56) sha1_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i*8));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

static void base64(const uint8_t *in, size_t len, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)in[i] << 16;
        if (i+1 < len) n |= (uint32_t)in[i+1] << 8;
        if (i+2 < len) n |= in[i+2];
        out[o++] = t[(n >> 18) & 63];
        out[o++] = t[(n >> 12) & 63];
        out[o++] = (i+1 < len) ? t[(n >> 6) & 63] : '=';
        out[o++] = (i+2 < len) ? t[n & 63] : '=';
    }
    out[o] = '\0';
}

/* ======================= server state ==================================== */

struct web_client { int fd; struct web_client *next; };

struct web_server {
    gpio_model *model;
    int         port;
    char        webroot[512];
    char        board[32];

    int         listen_fd;
    pthread_t   accept_thr;
    pthread_t   bcast_thr;
    volatile int running;

    pthread_mutex_t   clients_lock;
    struct web_client *clients;

    pthread_mutex_t notify_lock;
    pthread_cond_t  notify_cond;
    int             dirty;

    gpio_subscriber sub;
};

/* ======================= JSON state snapshot ============================= */

/* Build the state JSON into a static-size buffer (28 pins fits easily). Locks the
 * model internally; never call with the model lock already held. */
static void build_state(web_server *s, char *out, size_t cap)
{
    gpio_model *m = s->model;
    gpio_model_lock(m);
    int base = gpio_model_base(m), ngpio = gpio_model_ngpio(m);
    const char *label = gpio_model_label(m);

    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o,
            "{\"board\":\"%s\",\"label\":\"%s\",\"base\":%d,\"ngpio\":%d,\"pins\":[",
            s->board, label, base, ngpio);
    for (int i = 0; i < ngpio && o < cap; i++) {
        gpio_line *l = gpio_model_line(m, base + i);
        o += (size_t)snprintf(out + o, cap - o,
                "%s{\"n\":%d,\"exported\":%s,\"direction\":\"%s\",\"value\":%d,"
                "\"phys\":%d,\"active_low\":%s,\"edge\":\"%s\"}",
                i ? "," : "",
                l->number,
                l->exported ? "true" : "false",
                l->direction == GPIO_DIR_OUT ? "out" : "in",
                gpio_line_logical(l),
                l->phys,
                l->active_low ? "true" : "false",
                gpio_edge_to_str(l->edge));
    }
    gpio_model_unlock(m);
    if (o < cap) snprintf(out + o, cap - o, "]}");
}

/* ======================= WebSocket framing =============================== */

/* Send one unmasked text frame (server->client). Returns 0 or -1 on error. */
static int ws_send_text(int fd, const char *msg, size_t len)
{
    uint8_t hdr[10];
    size_t hn = 0;
    hdr[hn++] = 0x81;                 /* FIN + text opcode */
    if (len < 126) {
        hdr[hn++] = (uint8_t)len;
    } else if (len < 65536) {
        hdr[hn++] = 126;
        hdr[hn++] = (uint8_t)(len >> 8);
        hdr[hn++] = (uint8_t)(len & 0xff);
    } else {
        hdr[hn++] = 127;
        for (int i = 7; i >= 0; i--) hdr[hn++] = (uint8_t)(len >> (i*8));
    }
    if (send(fd, hdr, hn, MSG_NOSIGNAL) != (ssize_t)hn) return -1;
    if (len && send(fd, msg, len, MSG_NOSIGNAL) != (ssize_t)len) return -1;
    return 0;
}

/* ======================= HTTP helpers ==================================== */

static void send_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n <= 0) return;
        buf += n; len -= (size_t)n;
    }
}

static void http_send(int fd, const char *status, const char *ctype,
                      const char *body, size_t blen)
{
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status, ctype, blen);
    send_all(fd, hdr, (size_t)hn);
    if (body && blen) send_all(fd, body, blen);
}

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

/* Serve a whitelisted static file from webroot (no path traversal possible: the
 * caller maps request paths to fixed filenames). */
static void serve_file(web_server *s, int fd, const char *name)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", s->webroot, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        const char *msg = "not found";
        http_send(fd, "404 Not Found", "text/plain", msg, strlen(msg));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char *body = malloc((size_t)sz + 1);
    size_t rd = body ? fread(body, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (!body) { http_send(fd, "500 Internal Server Error", "text/plain", "", 0); return; }
    http_send(fd, "200 OK", mime_for(name), body, rd);
    free(body);
}

/* Case-insensitive header lookup within a NUL-terminated header block. Copies the
 * value into `out`. Returns 1 if found. */
static int header_val(const char *headers, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = headers;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > klen && strncasecmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (*v == ' ') v++;
            size_t vlen = (size_t)(eol - v);
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (*eol == '\0') break;
        p = eol + 2;
    }
    return 0;
}

/* ======================= client registry ================================= */

static void clients_add(web_server *s, int fd)
{
    struct web_client *c = malloc(sizeof(*c));
    if (!c) return;
    c->fd = fd;
    pthread_mutex_lock(&s->clients_lock);
    c->next = s->clients;
    s->clients = c;
    pthread_mutex_unlock(&s->clients_lock);
}

static void signal_dirty(web_server *s)
{
    pthread_mutex_lock(&s->notify_lock);
    s->dirty = 1;
    pthread_cond_signal(&s->notify_cond);
    pthread_mutex_unlock(&s->notify_lock);
}

/* ======================= routing ========================================= */

/* Parse an input level out of a POST body: JSON {"level":1}, or a bare "0"/"1". */
static int parse_level(const char *body)
{
    const char *p = strstr(body, "level");
    if (p) { p = strchr(p, ':'); if (p) p++; }
    else p = body;
    while (p && (*p == ' ' || *p == '"')) p++;
    if (!p) return -1;
    if (*p == '0') return 0;
    if (*p == '1') return 1;
    return -1;
}

static void handle_request(web_server *s, int fd, const char *method,
                           const char *path, const char *headers, const char *body)
{
    /* WebSocket upgrade? */
    char up[64], key[128];
    if (header_val(headers, "Upgrade", up, sizeof(up)) && strcasecmp(up, "websocket") == 0 &&
        header_val(headers, "Sec-WebSocket-Key", key, sizeof(key))) {
        char cat[200];
        snprintf(cat, sizeof(cat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        uint8_t digest[20];
        sha1_ctx c; sha1_init(&c); sha1_update(&c, cat, strlen(cat)); sha1_final(&c, digest);
        char accept[32];
        base64(digest, 20, accept);
        char resp[256];
        int rn = snprintf(resp, sizeof(resp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
        send_all(fd, resp, (size_t)rn);
        clients_add(s, fd);         /* now owned by the broadcaster */
        signal_dirty(s);            /* push current state to the new client */
        return;                     /* do NOT close fd */
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0)            { serve_file(s, fd, "index.html"); close(fd); return; }
        if (strcmp(path, "/style.css") == 0)   { serve_file(s, fd, "style.css");  close(fd); return; }
        if (strcmp(path, "/app.js") == 0)      { serve_file(s, fd, "app.js");     close(fd); return; }
        if (strcmp(path, "/favicon.svg") == 0) { serve_file(s, fd, "favicon.svg");close(fd); return; }
        if (strcmp(path, "/api/state") == 0) {
            char st[8192]; build_state(s, st, sizeof(st));
            http_send(fd, "200 OK", "application/json", st, strlen(st));
            close(fd); return;
        }
        http_send(fd, "404 Not Found", "text/plain", "not found", 9);
        close(fd); return;
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/reboot") == 0) {
            gpio_model_reboot(s->model);
            http_send(fd, "200 OK", "application/json", "{\"ok\":true}", 11);
            close(fd); return;
        }
        if (strncmp(path, "/api/pin/", 9) == 0) {
            int n = atoi(path + 9);
            int level = parse_level(body ? body : "");
            if (level < 0) {
                http_send(fd, "400 Bad Request", "application/json",
                          "{\"error\":\"level must be 0 or 1\"}", 31);
                close(fd); return;
            }
            int rc = gpio_model_drive_line(s->model, n, level);
            if (rc == 0) {
                char st[8192]; build_state(s, st, sizeof(st));
                http_send(fd, "200 OK", "application/json", st, strlen(st));
            } else if (rc == -EPERM) {
                http_send(fd, "409 Conflict", "application/json",
                          "{\"error\":\"pin is an output; driven by code, not the panel\"}", 58);
            } else {
                http_send(fd, "404 Not Found", "application/json",
                          "{\"error\":\"pin not exported\"}", 28);
            }
            close(fd); return;
        }
        http_send(fd, "404 Not Found", "text/plain", "not found", 9);
        close(fd); return;
    }

    http_send(fd, "405 Method Not Allowed", "text/plain", "", 0);
    close(fd);
}

/* ======================= connection + accept ============================= */

static void handle_conn(web_server *s, int fd)
{
    /* Read request headers (until CRLFCRLF), then any body per Content-Length. */
    char buf[16384];
    size_t used = 0;
    char *hdr_end = NULL;
    while (used < sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (n <= 0) { close(fd); return; }
        used += (size_t)n;
        buf[used] = '\0';
        if ((hdr_end = strstr(buf, "\r\n\r\n")) != NULL) break;
    }
    if (!hdr_end) { close(fd); return; }

    /* Request line */
    char method[8] = {0}, path[1024] = {0};
    if (sscanf(buf, "%7s %1023s", method, path) != 2) { close(fd); return; }

    const char *headers = strstr(buf, "\r\n");
    headers = headers ? headers + 2 : buf;
    *hdr_end = '\0';                                   /* terminate header block */
    const char *body = hdr_end + 4;
    size_t have_body = used - (size_t)(body - buf);

    /* Pull in the rest of the body if Content-Length says there's more. */
    char clv[16];
    if (header_val(headers, "Content-Length", clv, sizeof(clv))) {
        size_t want = (size_t)atoi(clv);
        while (have_body < want && used < sizeof(buf) - 1) {
            ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
            if (n <= 0) break;
            used += (size_t)n; buf[used] = '\0'; have_body += (size_t)n;
        }
    }

    handle_request(s, fd, method, path, headers, body);
}

struct conn_arg { web_server *s; int fd; };

static void *conn_thread(void *arg)
{
    struct conn_arg *a = arg;
    handle_conn(a->s, a->fd);
    free(a);
    return NULL;
}

static void *accept_thread(void *arg)
{
    web_server *s = arg;
    while (s->running) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }

        /* Bound slow/dead sends so the broadcaster can't stall forever. */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct conn_arg *a = malloc(sizeof(*a));
        if (!a) { close(fd); continue; }
        a->s = s; a->fd = fd;
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, a) != 0) { close(fd); free(a); continue; }
        pthread_detach(t);
    }
    return NULL;
}

/* Broadcaster: coalesce change notifications and push state to all WS clients. */
static void *bcast_thread(void *arg)
{
    web_server *s = arg;
    char st[8192];
    while (s->running) {
        pthread_mutex_lock(&s->notify_lock);
        while (!s->dirty && s->running)
            pthread_cond_wait(&s->notify_cond, &s->notify_lock);
        s->dirty = 0;
        pthread_mutex_unlock(&s->notify_lock);
        if (!s->running) break;

        build_state(s, st, sizeof(st));
        size_t len = strlen(st);

        pthread_mutex_lock(&s->clients_lock);
        struct web_client **pp = &s->clients;
        while (*pp) {
            struct web_client *c = *pp;
            if (ws_send_text(c->fd, st, len) < 0) {   /* dead — reap it */
                *pp = c->next;
                close(c->fd);
                free(c);
            } else {
                pp = &c->next;
            }
        }
        pthread_mutex_unlock(&s->clients_lock);
    }
    return NULL;
}

/* ======================= model subscriber =============================== */

static void web_on_change(gpio_model *m, gpio_line *l, int o, int nw, bool em, void *ud)
{ (void)m; (void)l; (void)o; (void)nw; (void)em; signal_dirty(ud); }

static void web_on_export(gpio_model *m, gpio_line *l, bool ex, void *ud)
{ (void)m; (void)l; (void)ex; signal_dirty(ud); }

/* ======================= lifecycle ====================================== */

web_server *web_server_start(gpio_model *model, int port,
                            const char *webroot, const char *board)
{
    web_server *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->model = model;
    s->port = port;
    snprintf(s->webroot, sizeof(s->webroot), "%s", webroot ? webroot : "web");
    snprintf(s->board, sizeof(s->board), "%s", board ? board : "classic");
    pthread_mutex_init(&s->clients_lock, NULL);
    pthread_mutex_init(&s->notify_lock, NULL);
    pthread_cond_init(&s->notify_cond, NULL);

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) { LOG_ERR("web: socket: %s", strerror(errno)); free(s); return NULL; }
    int one = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("web: bind :%d: %s", port, strerror(errno));
        close(s->listen_fd); free(s); return NULL;
    }
    if (listen(s->listen_fd, 16) < 0) {
        LOG_ERR("web: listen: %s", strerror(errno));
        close(s->listen_fd); free(s); return NULL;
    }

    /* Subscribe for change → broadcast. */
    s->sub.on_change = web_on_change;
    s->sub.on_export = web_on_export;
    s->sub.ud = s;
    gpio_model_subscribe(model, &s->sub);

    s->running = 1;
    if (pthread_create(&s->bcast_thr, NULL, bcast_thread, s) != 0 ||
        pthread_create(&s->accept_thr, NULL, accept_thread, s) != 0) {
        LOG_ERR("web: pthread_create failed");
        s->running = 0;
        close(s->listen_fd); free(s); return NULL;
    }

    LOG_INFO("web: panel + API on http://0.0.0.0:%d (webroot %s)", port, s->webroot);
    return s;
}

void web_server_stop(web_server *s)
{
    if (!s) return;
    s->running = 0;
    shutdown(s->listen_fd, SHUT_RDWR);
    close(s->listen_fd);
    signal_dirty(s);                 /* wake the broadcaster so it can exit */
    pthread_join(s->accept_thr, NULL);
    pthread_join(s->bcast_thr, NULL);

    pthread_mutex_lock(&s->clients_lock);
    for (struct web_client *c = s->clients; c; ) {
        struct web_client *n = c->next; close(c->fd); free(c); c = n;
    }
    pthread_mutex_unlock(&s->clients_lock);
    free(s);
}

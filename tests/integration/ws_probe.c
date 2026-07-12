/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * ws_probe.c — validates the WebSocket endpoint end-to-end.
 *
 * Uses the RFC 6455 example handshake key so the expected Sec-WebSocket-Accept is a
 * known constant — this checks the server's SHA-1 + base64 are byte-exact, the same
 * computation a browser does. Then it reads the initial state frame the server
 * pushes on connect, proving the live broadcast path works.
 *
 *   key    "dGhlIHNhbXBsZSBub25jZQ=="
 *   accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="   (RFC 6455 §1.3)
 *
 * Exit: 0 = handshake correct AND a text frame received; 1 = failure.
 * Usage: ws_probe <host> <port>
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define EXPECT_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s host port\n", argv[0]); return 1; }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
    if (getaddrinfo(argv[1], argv[2], &hints, &ai) != 0) { perror("getaddrinfo"); return 1; }
    int fd = socket(ai->ai_family, ai->ai_socktype, 0);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) { perror("connect"); return 1; }
    freeaddrinfo(ai);

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[512];
    int rn = snprintf(req, sizeof(req),
        "GET /ws HTTP/1.1\r\nHost: %s:%s\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", argv[1], argv[2]);
    if (send(fd, req, rn, 0) != rn) { perror("send"); return 1; }

    /* Accumulate the handshake response plus whatever frame bytes follow. */
    unsigned char buf[8192];
    size_t used = 0;
    char *hdr_end = NULL;
    /* Read until we have the handshake response AND enough of the following frame to
     * parse its header + a little payload. The handshake and the frame can arrive in
     * separate TCP segments, so we can't assume one recv() gets both. */
    while (used < sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (n <= 0) break;                       /* EOF or timeout */
        used += (size_t)n;
        buf[used] = '\0';
        if (!hdr_end)
            hdr_end = strstr((char *)buf, "\r\n\r\n");
        if (hdr_end) {
            size_t frame_bytes = used - (size_t)((hdr_end + 4) - (char *)buf);
            if (frame_bytes >= 12)               /* enough for header + preview */
                break;
        }
    }
    if (!hdr_end) { fprintf(stderr, "no handshake response\n"); return 1; }

    if (!strstr((char *)buf, "101 Switching Protocols")) {
        fprintf(stderr, "not a 101 response\n"); return 1;
    }
    if (!strstr((char *)buf, "Sec-WebSocket-Accept: " EXPECT_ACCEPT)) {
        fprintf(stderr, "wrong/missing Sec-WebSocket-Accept (SHA1/base64 mismatch)\n");
        return 1;
    }

    /* Parse the first server->client frame that follows the header. Getting a text
     * opcode is proof the live-state push works; the payload preview is best-effort. */
    unsigned char *fp = (unsigned char *)hdr_end + 4;
    size_t avail = used - (size_t)(fp - buf);
    if (avail < 2) { fprintf(stderr, "no frame after handshake\n"); return 1; }
    int opcode = fp[0] & 0x0f;
    if (opcode != 0x1) { fprintf(stderr, "first frame not text (opcode=%d)\n", opcode); return 1; }

    size_t lenfield = fp[1] & 0x7f;
    size_t off = 2, len = lenfield;
    if (lenfield == 126 && avail >= 4) { len = ((size_t)fp[2] << 8) | fp[3]; off = 4; }
    else if (lenfield == 127)          { off = 10; }   /* not used by our payloads */

    char preview[48] = {0};
    size_t cp = (avail > off) ? avail - off : 0;
    if (cp > sizeof(preview) - 1) cp = sizeof(preview) - 1;
    if (cp) memcpy(preview, fp + off, cp);
    printf("WSOK accept-valid, text frame len>=%zu: %s...\n", len, preview);
    close(fd);
    return 0;
}

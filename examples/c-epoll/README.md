# C — blink + button with `epoll`

Direct sysfs access with `open`/`read`/`write`, and `epoll` with `EPOLLPRI` on
`gpio27/value` for edge detection. This is the canonical proof of poll fidelity: the
`epoll_wait()` call blocks until GPIO27 sees a **rising** edge.

```sh
make
./blink-button
```

Expected output:

```
Blinking GPIO17...
WAITING for button press on GPIO27 (up to 5s)...
Button pressed! value=1
```

Raise GPIO27 while it waits — click it on the panel, or:

```sh
curl -X POST -d '{"level":1}' http://localhost:8080/api/pin/27
```

The `lseek(fd, 0, SEEK_SET)` + `read()` after each event is how sysfs edge interrupts
are re-armed; without it `poll`/`epoll` would return immediately every time.

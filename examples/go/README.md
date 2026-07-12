# Go — blink + button with the standard library (optional)

Direct sysfs file I/O plus `syscall.Select` with the `value` fd in the **exception**
set — how sysfs GPIO edges surface to `select(2)`, equivalent to `poll()`'s `POLLPRI`.
Standard library only; no modules to fetch.

```sh
go run blink-button.go
# or: go build -o blink-button . && ./blink-button
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

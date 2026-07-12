# Shell — blink + button with plain `echo`

The classic teaching example: everything is a file write.

```sh
sh blink-button.sh
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

The whole GPIO lifecycle is visible here:

```sh
echo 17  > /sys/class/gpio/export           # create gpio17/
echo out > /sys/class/gpio/gpio17/direction # make it an output
echo 1   > /sys/class/gpio/gpio17/value     # drive it high
echo 17  > /sys/class/gpio/unexport         # release it
```

POSIX shell can't `poll(2)`, so the button is watched with a short read loop. For true
edge-triggered detection, see [`../c-epoll`](../c-epoll) or [`../python-sysfs`](../python-sysfs).

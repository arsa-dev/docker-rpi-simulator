# Node.js — blink + button with `onoff`

[`onoff`](https://www.npmjs.com/package/onoff) is a widely used Node GPIO library that
talks to the **sysfs** interface and does edge detection with `epoll` on the `value`
file — so it runs unchanged against this simulator. This is the strongest proof that
"real-world sysfs code just works."

```sh
npm install
node blink-button.js
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

`new Gpio(27, 'in', 'rising')` sets `direction=in` and `edge=rising`, then
`button.watch(cb)` arms an epoll on `gpio27/value` — the callback fires on the edge.

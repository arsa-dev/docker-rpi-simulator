// SPDX-License-Identifier: AGPL-3.0-or-later
// blink-button.js — blink an LED and wait for a button using `onoff`, a real-world
// Node GPIO library that talks to the sysfs interface (/sys/class/gpio) and does edge
// detection with epoll on the value file. It runs unchanged against this simulator.
//
//   LED    = GPIO17 (output)
//   BUTTON = GPIO27 (input, rising edge)
//
// Run:  npm install && node blink-button.js

const { Gpio } = require('onoff');

const led = new Gpio(17, 'out');
const button = new Gpio(27, 'in', 'rising');   // watch the rising edge

function cleanup(code) {
  led.unexport();
  button.unexport();
  process.exit(code);
}

// --- blink, then arm the button ---
console.log('Blinking GPIO17...');
let ticks = 0;
const blink = setInterval(() => {
  led.writeSync(led.readSync() ^ 1);
  if (++ticks === 6) {
    clearInterval(blink);
    led.writeSync(0);
    armButton();
  }
}, 100);

function armButton() {
  console.log('WAITING for button press on GPIO27 (up to 5s)...');

  const timer = setTimeout(() => {
    console.log('timeout: no button press');
    cleanup(3);
  }, 5000);

  button.watch((err, value) => {
    if (err) { console.error(err); cleanup(1); }
    clearTimeout(timer);
    console.log(`Button pressed! value=${value}`);
    cleanup(0);
  });
}

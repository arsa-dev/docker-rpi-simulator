/* gpio-sim panel — renders the 40-pin header, reflects live state over WebSocket,
   and lets you operate input pins. No external dependencies. */
'use strict';

/* The physical Raspberry Pi 40-pin (J8) header, in physical pin order 1..40.
   type: 'gpio' | '3v3' | '5v' | 'gnd'. For gpio, bcm is the BCM number and alt is
   the common alternate-function name. */
const HEADER = [
  { p: 1,  type: '3v3' },                         { p: 2,  type: '5v' },
  { p: 3,  type: 'gpio', bcm: 2,  alt: 'SDA1' },   { p: 4,  type: '5v' },
  { p: 5,  type: 'gpio', bcm: 3,  alt: 'SCL1' },   { p: 6,  type: 'gnd' },
  { p: 7,  type: 'gpio', bcm: 4,  alt: 'GPCLK0' }, { p: 8,  type: 'gpio', bcm: 14, alt: 'TXD' },
  { p: 9,  type: 'gnd' },                          { p: 10, type: 'gpio', bcm: 15, alt: 'RXD' },
  { p: 11, type: 'gpio', bcm: 17 },                { p: 12, type: 'gpio', bcm: 18, alt: 'PCM_CLK' },
  { p: 13, type: 'gpio', bcm: 27 },                { p: 14, type: 'gnd' },
  { p: 15, type: 'gpio', bcm: 22 },                { p: 16, type: 'gpio', bcm: 23 },
  { p: 17, type: '3v3' },                          { p: 18, type: 'gpio', bcm: 24 },
  { p: 19, type: 'gpio', bcm: 10, alt: 'MOSI' },   { p: 20, type: 'gnd' },
  { p: 21, type: 'gpio', bcm: 9,  alt: 'MISO' },   { p: 22, type: 'gpio', bcm: 25 },
  { p: 23, type: 'gpio', bcm: 11, alt: 'SCLK' },   { p: 24, type: 'gpio', bcm: 8,  alt: 'CE0' },
  { p: 25, type: 'gnd' },                          { p: 26, type: 'gpio', bcm: 7,  alt: 'CE1' },
  { p: 27, type: 'gpio', bcm: 0,  alt: 'ID_SD' },  { p: 28, type: 'gpio', bcm: 1,  alt: 'ID_SC' },
  { p: 29, type: 'gpio', bcm: 5 },                 { p: 30, type: 'gnd' },
  { p: 31, type: 'gpio', bcm: 6 },                 { p: 32, type: 'gpio', bcm: 12, alt: 'PWM0' },
  { p: 33, type: 'gpio', bcm: 13, alt: 'PWM1' },   { p: 34, type: 'gnd' },
  { p: 35, type: 'gpio', bcm: 19, alt: 'PCM_FS' }, { p: 36, type: 'gpio', bcm: 16 },
  { p: 37, type: 'gpio', bcm: 26 },                { p: 38, type: 'gpio', bcm: 20, alt: 'PCM_DIN' },
  { p: 39, type: 'gnd' },                          { p: 40, type: 'gpio', bcm: 21, alt: 'PCM_DOUT' },
];

const grid = document.getElementById('grid');
const el = {};            // bcm number -> { pin, dot, badge }
let pinState = {};        // bcm number -> state object from server

/* ---- build the static grid once ---- */
function buildGrid() {
  HEADER.forEach((h, i) => {
    const cell = document.createElement('div');
    cell.className = 'pin ' + (i % 2 === 0 ? 'left' : 'right');
    cell.setAttribute('role', 'listitem');

    const phys = document.createElement('span');
    phys.className = 'phys';
    phys.textContent = h.p;

    const dot = document.createElement('span');
    dot.className = 'dot';

    const labels = document.createElement('span');
    labels.className = 'labels';
    const name = document.createElement('div');
    name.className = 'name';
    const alt = document.createElement('div');
    alt.className = 'alt';
    labels.append(name, alt);

    if (h.type === 'gpio') {
      name.textContent = 'GPIO' + h.bcm;
      alt.textContent = h.alt || ('BCM ' + h.bcm);
      cell.dataset.bcm = h.bcm;
      el[h.bcm] = { pin: cell, dot, name, alt };
    } else if (h.type === 'gnd') {
      name.textContent = 'GND'; name.classList.add('gnd-t');
      dot.classList.add('gnd'); alt.textContent = 'ground';
    } else { /* power */
      name.textContent = h.type === '3v3' ? '3V3' : '5V';
      name.classList.add('pwr-t'); dot.classList.add('pwr');
      alt.textContent = 'power';
    }

    cell.append(phys, dot, labels);
    grid.append(cell);
  });
}

/* ---- apply server state to the grid ---- */
function render() {
  HEADER.forEach(h => {
    if (h.type !== 'gpio') return;
    const ui = el[h.bcm];
    const s = pinState[h.bcm];
    const { pin, dot, alt } = ui;

    // reset classes we manage
    dot.className = 'dot';
    pin.classList.remove('dim', 'interactive');
    pin.removeAttribute('tabindex');
    pin.removeAttribute('role');
    pin.setAttribute('role', 'listitem');

    if (!s || !s.exported) {
      pin.classList.add('dim');
      dot.classList.add('dim');
      alt.textContent = h.alt || ('BCM ' + h.bcm);
      pin.setAttribute('aria-label', `GPIO${h.bcm}, not exported`);
      return;
    }

    const high = s.value === 1;
    if (s.direction === 'out') {
      dot.classList.add('out'); if (high) dot.classList.add('on');
      alt.textContent = 'out' + (s.active_low ? ' ·⌐' : '');
      pin.setAttribute('tabindex', '0');
      pin.setAttribute('role', 'status');
      pin.setAttribute('aria-label',
        `GPIO${h.bcm}, output, ${high ? 'high' : 'low'} (driven by code)`);
    } else {
      dot.classList.add('in'); if (high) dot.classList.add('on');
      const edge = s.edge && s.edge !== 'none' ? ` ·${s.edge}` : '';
      alt.textContent = 'in' + (s.active_low ? ' ·⌐' : '') + edge;
      pin.classList.add('interactive');
      pin.setAttribute('tabindex', '0');
      pin.setAttribute('role', 'switch');
      pin.setAttribute('aria-checked', high ? 'true' : 'false');
      pin.setAttribute('aria-label', `GPIO${h.bcm}, input, ${high ? 'high' : 'low'} — toggle`);
    }
  });
}

/* ---- operate an input pin ---- */
async function toggleInput(bcm) {
  const s = pinState[bcm];
  if (!s || !s.exported || s.direction !== 'in') return;
  const next = s.value === 1 ? 0 : 1;
  try {
    await fetch('/api/pin/' + bcm, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ level: next }),
    });
    // The authoritative update arrives over the WebSocket; no optimistic write.
  } catch (e) { /* ignore; state stays as the server last reported */ }
}

grid.addEventListener('click', ev => {
  const cell = ev.target.closest('.pin.interactive');
  if (cell) toggleInput(Number(cell.dataset.bcm));
});
grid.addEventListener('keydown', ev => {
  if (ev.key !== ' ' && ev.key !== 'Enter') return;
  const cell = ev.target.closest('.pin.interactive');
  if (cell) { ev.preventDefault(); toggleInput(Number(cell.dataset.bcm)); }
});

document.getElementById('reboot').addEventListener('click', () => {
  fetch('/api/reboot', { method: 'POST' }).catch(() => {});
});

/* ---- ingest a state snapshot ---- */
function applyState(st) {
  document.getElementById('m-board').textContent = st.board || '—';
  document.getElementById('m-label').textContent = st.label || '—';
  pinState = {};
  (st.pins || []).forEach(p => { pinState[p.n] = p; });
  render();
}

/* ---- WebSocket with reconnect ---- */
function setLink(up) {
  const m = document.getElementById('m-link');
  m.textContent = up ? 'connected' : 'reconnecting…';
  m.className = up ? 'link-up' : 'link-down';
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => setLink(true);
  ws.onmessage = ev => { try { applyState(JSON.parse(ev.data)); } catch (e) {} };
  ws.onclose = () => { setLink(false); setTimeout(connect, 1000); };
  ws.onerror = () => ws.close();
}

/* ---- boot ---- */
buildGrid();
fetch('/api/state').then(r => r.json()).then(applyState).catch(() => {});
connect();

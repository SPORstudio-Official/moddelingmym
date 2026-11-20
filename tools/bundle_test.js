/* ==========================================================================
 *  Single-file bundle test
 *
 *  Loads dist/AetherOS.html the way a browser would -- parses out its <script>
 *  blocks, runs them against a fake DOM -- and proves that:
 *
 *    1. it declares no external reference at all (it must work from file://);
 *    2. the embedded module boots with no network access;
 *    3. it renders, and typing reaches the OS.
 *
 *  Run: node tools/bundle_test.js [--ppm out.ppm]
 * ======================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.join(__dirname, '..');
const FILE = path.join(ROOT, 'dist', 'AetherOS.html');

let failures = 0;
function check(name, cond, detail) {
  if (cond) console.log('  ok   ' + name);
  else { console.log('  FAIL ' + name + (detail ? '  -- ' + detail : '')); failures++; }
}

if (!fs.existsSync(FILE)) {
  console.error('dist/AetherOS.html missing -- run: node tools/bundle_standalone.js');
  process.exit(1);
}

const html = fs.readFileSync(FILE, 'utf8');
console.log('AetherOS single-file test\n');

/* ---- 1. self-containment ---------------------------------------------- */
const bare = html
  .replace(/<script>[\s\S]*?<\/script>/g, '')     /* inline code is fine */
  .replace(/<style>[\s\S]*?<\/style>/g, '');
check('no external src/href attributes',
  !/\b(?:src|href)\s*=\s*["'](?!#|data:)/i.test(bare));
check('no absolute URLs in markup',
  !/https?:\/\//i.test(bare));

const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
check('three inline script blocks (loader, host, boot)', scripts.length === 3,
  scripts.length + ' found');

/* ---- 2. fake DOM ------------------------------------------------------- */
const listeners = new Map();
function mk(n) {
  return {
    _name: n, width: 1280, height: 720, focus() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: 640, height: 360 }; },
    addEventListener(t, f) {
      const k = n + ':' + t;
      if (!listeners.has(k)) listeners.set(k, []);
      listeners.get(k).push(f);
    },
    getContext() {
      if (!this._c) {
        this._c = {
          createImageData(w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; },
          putImageData(i) { this.last = i; },
        };
      }
      return this._c;
    },
  };
}
const canvas = mk('canvas');
const els = {
  screen: canvas,
  status: { textContent: '' },
  fps: { textContent: '' },
  overlay: { classList: { add() {}, remove() {} } },
  'err-title': { textContent: '' }, 'err-body': { textContent: '' },
  'err-hint': { textContent: '' }, 'err-close': { focus() {}, addEventListener() {} },
  fullscreen: { addEventListener() {} },
};

const sandbox = {
  console,
  performance: { now: () => Number(process.hrtime.bigint() / 1000n) / 1000 },
  requestAnimationFrame: (f) => setTimeout(() => f(sandbox.performance.now()), 0),
  setTimeout, clearTimeout,
  document: {
    title: '', fullscreenElement: null,
    getElementById: (id) => els[id] || null,
    addEventListener() {},
    documentElement: { requestFullscreen: () => Promise.resolve() },
    exitFullscreen: () => Promise.resolve(),
  },
  atob: (s) => Buffer.from(s, 'base64').toString('binary'),
  AudioContext: function () {
    return {
      createOscillator: () => ({ frequency: {}, connect: () => ({ connect() {} }), start() {}, stop() {} }),
      createGain: () => ({ gain: {}, connect: () => ({ connect() {} }) }),
      destination: {}, currentTime: 0,
    };
  },
  WebAssembly, BigInt, Math, Promise, Date, Object, String, Number, isNaN, parseInt,
  Uint8Array, Uint8ClampedArray, Uint32Array, Int32Array, DataView, ArrayBuffer,
};
sandbox.window = sandbox;          /* in a browser `window` *is* the global */
sandbox.self = sandbox;
sandbox.globalThis = sandbox;
sandbox.addEventListener = (t, f) => {
  const k = 'window:' + t;
  if (!listeners.has(k)) listeners.set(k, []);
  listeners.get(k).push(f);
};
/* No fetch: the bundle must not need one.  If the loader fails to install its
   own, the boot reports "fetch is not defined" and the test fails. */
sandbox.fetch = undefined;

vm.createContext(sandbox);
for (const s of scripts) vm.runInContext(s, sandbox, { filename: 'AetherOS.html' });

function fire(target, type, ev) {
  for (const f of listeners.get(target._name + ':' + type) || []) {
    f(Object.assign({ preventDefault() {}, target }, ev));
  }
}

/* ---- 3. run ------------------------------------------------------------ */
setTimeout(() => {
  const ctx = canvas.getContext('2d');
  if (!ctx.last) {
    check('booted and painted', false, 'status="' + els.status.textContent + '"');
    console.log('\n' + failures + ' CHECK(S) FAILED');
    process.exit(1);
  }

  let nonBlack = 0;
  for (let i = 0; i < ctx.last.data.length; i += 4) {
    if (ctx.last.data[i] || ctx.last.data[i + 1] || ctx.last.data[i + 2]) nonBlack++;
  }
  check('booted from a single file with no network', els.status.textContent === 'running',
    'status="' + els.status.textContent + '"');
  check('rendered the desktop', nonBlack > 300000, nonBlack + ' non-black px');

  const before = new Uint8ClampedArray(ctx.last.data);
  for (const ch of 'abc') {
    const code = 'Key' + ch.toUpperCase();
    fire(canvas, 'keydown', { key: ch, code, ctrlKey: false, altKey: false, metaKey: false, repeat: false });
    fire(canvas, 'keyup', { key: ch, code });
  }

  setTimeout(() => {
    const after = ctx.last.data;
    let changed = 0;
    for (let i = 0; i < before.length; i += 4) {
      if (before[i] !== after[i] || before[i + 1] !== after[i + 1] || before[i + 2] !== after[i + 2]) changed++;
    }
    check('typing reaches the OS', changed > 100, changed + ' px changed');

    const ppmIdx = process.argv.indexOf('--ppm');
    if (ppmIdx > 0 && process.argv[ppmIdx + 1]) {
      const out = Buffer.alloc(1280 * 720 * 3);
      for (let i = 0, o = 0; i < 1280 * 720; i++, o += 4) {
        out[i * 3] = after[o]; out[i * 3 + 1] = after[o + 1]; out[i * 3 + 2] = after[o + 2];
      }
      fs.writeFileSync(process.argv[ppmIdx + 1],
        Buffer.concat([Buffer.from('P6\n1280 720\n255\n', 'ascii'), out]));
    }

    console.log(failures === 0 ? '\nALL CHECKS PASSED' : '\n' + failures + ' CHECK(S) FAILED');
    process.exit(failures === 0 ? 0 : 1);
  }, 700);
}, 4600);

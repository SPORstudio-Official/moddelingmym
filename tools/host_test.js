/* ==========================================================================
 *  Host-bridge test harness
 *
 *  Electron's binary cannot be downloaded in every environment, so this proves
 *  the parts that actually matter by running the *real* app/aether-host.js
 *  against a minimal fake DOM and canvas:
 *
 *    1. the kernel boots through the shell's imports;
 *    2. js_present decorates the framebuffer in the RGBA order canvas wants;
 *    3. synthetic DOM keyboard and mouse input reach the kernel and visibly
 *       change what it renders -- i.e. the OS is genuinely interactive.
 *
 *  Run: node tools/host_test.js [--png out.ppm]
 * ======================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.join(__dirname, '..');
let failures = 0;
function check(name, cond, detail) {
  if (cond) console.log('  ok   ' + name);
  else { console.log('  FAIL ' + name + (detail ? '  -- ' + detail : '')); failures++; }
}

/* ---- fake DOM ---------------------------------------------------------- */
const listeners = new Map();
function mkTarget(name) {
  return {
    _name: name,
    width: 1280,
    height: 720,
    focus() {},
    /* Displayed at half size so the test also exercises DOM->guest scaling. */
    getBoundingClientRect() { return { left: 0, top: 0, width: 640, height: 360 }; },
    addEventListener(type, fn) {
      const k = name + ':' + type;
      if (!listeners.has(k)) listeners.set(k, []);
      listeners.get(k).push(fn);
    },
    getContext() {
      if (!this._ctx) {
        this._ctx = {
          createImageData(w, h) {
            return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) };
          },
          putImageData(img) { this.last = img; },
        };
      }
      return this._ctx;
    },
  };
}

const canvas = mkTarget('canvas');
const windowTarget = mkTarget('window');

function fire(target, type, ev) {
  for (const fn of listeners.get(target._name + ':' + type) || []) {
    fn(Object.assign({ preventDefault() {}, target }, ev));
  }
}

/* ---- sandbox ----------------------------------------------------------- */
const wasmPath = path.join(ROOT, 'app', 'aether.wasm');
const sandbox = {
  console,
  performance: { now: () => Number(process.hrtime.bigint() / 1000n) / 1000 },
  requestAnimationFrame: (fn) => setTimeout(() => fn(sandbox.performance.now()), 0),
  setTimeout, clearTimeout,
  document: { title: '', addEventListener() {} },
  window: windowTarget,
  fetch: async () => {
    const buf = fs.readFileSync(wasmPath);
    return {
      ok: true, status: 200,
      arrayBuffer: async () => buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength),
    };
  },
  WebAssembly, BigInt, Math,
  Uint8Array, Uint8ClampedArray, Uint32Array, Int32Array, DataView, ArrayBuffer,
};
sandbox.self = sandbox;
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(ROOT, 'app', 'aether-host.js'), 'utf8'),
  sandbox, { filename: 'aether-host.js' });
vm.runInContext('globalThis.__AetherHost = AetherHost;', sandbox);

/* ---- helpers ----------------------------------------------------------- */
const ctx = canvas.getContext('2d');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function snapshot() {
  /* Copy the painted image so later frames cannot mutate it underneath us. */
  return new Uint8ClampedArray(ctx.last.data);
}

function diffRegion(a, b, x0, y0, x1, y1) {
  let n = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const o = (y * 1280 + x) * 4;
      if (a[o] !== b[o] || a[o + 1] !== b[o + 1] || a[o + 2] !== b[o + 2]) n++;
    }
  }
  return n;
}

/* Sample only the terminal body: the taskbar clock and CPU meter animate on
   their own and would swamp a full-frame comparison. */
const TX0 = 290, TY0 = 150, TX1 = 1020, TY1 = 560;

function cropSig(img, x0, y0, x1, y1) {
  /* Coarse perceptual signature, robust to the blinking caret. */
  const out = [];
  for (let y = y0; y < y1; y += 2) {
    for (let x = x0; x < x1; x += 2) {
      const o = (y * 1280 + x) * 4;
      out.push(img[o] > 128 ? 1 : 0);
    }
  }
  return out;
}
function sigDiff(a, b) {
  let n = 0;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) n++;
  return n;
}

function typeText(s) {
  for (const ch of s) {
    const code = 'Key' + ch.toUpperCase();
    fire(canvas, 'keydown', { key: ch, code, ctrlKey: false, altKey: false, metaKey: false, repeat: false });
    fire(canvas, 'keyup', { key: ch, code });
  }
}
function pressKey(key, code) {
  fire(canvas, 'keydown', { key, code, ctrlKey: false, altKey: false, metaKey: false, repeat: false });
  fire(canvas, 'keyup', { key, code });
}

/* ---- run --------------------------------------------------------------- */
(async () => {
  console.log('AetherOS host-bridge test\n');

  const el = {
    canvas,
    status: { textContent: '' },
    fps: { textContent: '' },
    showError(title, body) {
      console.log('  [overlay] ' + title + ': ' + String(body).split('\n')[0]);
    },
  };

  const ok = await sandbox.__AetherHost.start(el);
  check('shell starts and boots the kernel', ok === true);
  if (!ok) { console.log('\nFAILED: boot did not complete'); process.exit(1); }

  /* The boot animation runs for ~2.65 s of emulated time; wait past it. */
  await sleep(4200);

  const img = ctx.last;
  check('js_present painted a 1280x720 ImageData',
    !!img && img.width === 1280 && img.height === 720);

  let opaque = 0, nonBlack = 0;
  for (let i = 0; i < img.data.length; i += 4) {
    if (img.data[i + 3] === 255) opaque++;
    if (img.data[i] || img.data[i + 1] || img.data[i + 2]) nonBlack++;
  }
  check('alpha channel is fully opaque', opaque === 1280 * 720, opaque + ' px');
  check('desktop rendered content', nonBlack > 400000, nonBlack + ' non-black px');

  /* ---- keyboard: type into AetherShell -------------------------------- */
  /* Sample the terminal body only; the taskbar clock and CPU meter animate on
     their own and would make any full-frame comparison meaningless. */
  const TX0 = 290, TY0 = 150, TX1 = 1020, TY1 = 560;
  const beforeKeys = snapshot();
  typeText('apps');
  pressKey('Enter', 'Enter');
  await sleep(500);
  const afterKeys = snapshot();
  const keyDelta = diffRegion(beforeKeys, afterKeys, TX0, TY0, TX1, TY1);
  check('typing reaches the kernel and changes the terminal',
    keyDelta > 400, keyDelta + ' px changed');

  /* ---- a real command round-trip -------------------------------------- */
  /* Type "help" and press Enter.  The shell prints a command list, which must
     add several lines of text to the terminal: a much larger change than the
     one line of echoed input.  This is the end-to-end proof that DOM input
     reaches an application and its output is rendered back. */
  const beforeCmd = cropSig(ctx.last.data, TX0, TY0, TX1, TY1);
  typeText('help');
  pressKey('Enter', 'Enter');
  await sleep(600);
  const afterCmd = cropSig(ctx.last.data, TX0, TY0, TX1, TY1);
  const cmdDiff = sigDiff(beforeCmd, afterCmd);
  check('running "help" produced command output', cmdDiff > 60, cmdDiff + ' cells changed');

  /* ---- mouse: click the Start button ---------------------------------- */
  /* Guest (44,687) is the Start button; the canvas is displayed at half scale,
     so the DOM coordinates are half that. */
  const beforeClick = snapshot();
  fire(canvas, 'mousemove', { clientX: 22, clientY: 343 });
  fire(canvas, 'mousedown', { clientX: 22, clientY: 343, button: 0 });
  fire(windowTarget, 'mouseup', { clientX: 22, clientY: 343, button: 0 });
  await sleep(500);
  const afterClick = snapshot();
  /* The Start menu opens in the bottom-left.  A mere pointer move would change
     only the ~200 px under the cursor, so require a much larger delta. */
  const clickDelta = diffRegion(beforeClick, afterClick, 0, 380, 400, 680);
  check('mouse click opens the Start menu', clickDelta > 3000, clickDelta + ' px changed');

  /* ---- coordinate scaling --------------------------------------------- */
  /* A click at DOM (22,343) must map to guest (44,686).  Verified implicitly
     above: (44,687) is the Start button and the menu opened. */
  check('DOM coordinates scale onto the 1280x720 surface', clickDelta > 3000);

  /* ---- optional screenshot -------------------------------------------- */
  const pngIdx = process.argv.indexOf('--ppm');
  if (pngIdx > 0 && process.argv[pngIdx + 1]) {
    const out = Buffer.alloc(1280 * 720 * 3);
    const px = afterClick;
    for (let i = 0, o = 0; i < 1280 * 720; i++, o += 4) {
      out[i * 3] = px[o]; out[i * 3 + 1] = px[o + 1]; out[i * 3 + 2] = px[o + 2];
    }
    fs.writeFileSync(process.argv[pngIdx + 1],
      Buffer.concat([Buffer.from('P6\n1280 720\n255\n', 'ascii'), out]));
    console.log('\nwrote ' + process.argv[pngIdx + 1]);
  }

  console.log(failures === 0 ? '\nALL CHECKS PASSED' : '\n' + failures + ' CHECK(S) FAILED');
  process.exit(failures === 0 ? 0 : 1);
})().catch((err) => {
  console.log('EXCEPTION', (err && err.stack) || err);
  process.exit(1);
});

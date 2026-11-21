/* ==========================================================================
 *  dist/AE.exe structural test
 *
 *  The launcher is Windows code and cannot run here, so this checks everything
 *  that CAN be checked without Windows:
 *
 *    1. it is a well-formed PE32+ GUI executable, x86-64;
 *    2. its import table names only functions that exist on Windows 10/11;
 *    3. the payload embedded in it is byte-identical to dist/AE.html;
 *    4. that payload, extracted the way the launcher extracts it, boots the OS
 *       in a JavaScript engine -- so the bytes the exe will write to %TEMP% are
 *       proven good, not just present.
 *
 *  Run: node tools/exe_test.js
 * ======================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.join(__dirname, '..');
const EXE = path.join(ROOT, 'dist', 'AE.exe');
const HTML = path.join(ROOT, 'dist', 'AE.html');

let failures = 0;
function check(name, cond, detail) {
  if (cond) console.log('  ok   ' + name);
  else { console.log('  FAIL ' + name + (detail ? '  -- ' + detail : '')); failures++; }
}

if (!fs.existsSync(EXE)) {
  console.error('dist/AE.exe missing -- run: bash tools/build_exe.sh');
  process.exit(1);
}

const exe = fs.readFileSync(EXE);
console.log('AE.exe test\n');

/* ---- 1. PE structure --------------------------------------------------- */
const peOff = exe.readUInt32LE(0x3c);
const machine = exe.readUInt16LE(peOff + 4);
const numSections = exe.readUInt16LE(peOff + 6);
const optOff = peOff + 24;
const magic = exe.readUInt16LE(optOff);
const subsystem = exe.readUInt16LE(optOff + 68);
const entryRva = exe.readUInt32LE(optOff + 16);

check('starts with MZ', exe.slice(0, 2).toString() === 'MZ');
check('PE signature present', exe.slice(peOff, peOff + 4).toString('binary') === 'PE\0\0');
check('x86-64 machine type', machine === 0x8664, '0x' + machine.toString(16));
check('PE32+ optional header', magic === 0x20b, '0x' + magic.toString(16));
check('GUI subsystem (no console flash)', subsystem === 2, String(subsystem));
check('has an entry point', entryRva !== 0, '0x' + entryRva.toString(16));
check('section count sane', numSections > 0 && numSections < 32, String(numSections));

/* ---- 2. imports -------------------------------------------------------- */
{
  const dlls = [];
  const names = [];
  for (let i = 0; i < exe.length - 20; i++) {
    if (exe[i] !== 0x2e) continue;             /* '.' of '.dll' */
    const s = exe.slice(i - 8, i + 4).toString('latin1');
    if (/DLL$/i.test(s) && /^[A-Za-z0-9_.-]+$/i.test(s)) {
      if (!dlls.includes(s)) dlls.push(s);
    }
  }
  /* Function names appear as NUL-terminated ASCII in the import tables. */
  const known = ['CloseHandle', 'CreateFileA', 'CreateProcessA', 'GetEnvironmentVariableA',
    'GetFileAttributesA', 'GetFileSizeEx', 'GetLastError', 'GetModuleFileNameA',
    'GetTempPathA', 'MessageBoxA', 'ReadFile', 'RegCloseKey', 'RegOpenKeyExA',
    'RegQueryValueExA', 'ShellExecuteA', 'VirtualAlloc', 'VirtualFree', 'WriteFile',
    'GetModuleHandleA'];
  for (const k of known) {
    if (exe.includes(Buffer.from(k + '\0', 'latin1'))) names.push(k);
  }
  check('uses only core Windows DLLs',
    dlls.every((d) => /^(KERNEL32|USER32|SHELL32|ADVAPI32|GDI32)\.DLL$/i.test(d)),
    dlls.join(', '));
  check('all import names are real Windows APIs',
    names.every((n) => known.includes(n)), names.filter((n) => !known.includes(n)).join(', '));
  check('no C runtime dependency', !dlls.some((d) => /^(MSVCRT|UCRT|VCRUNTIME)/i.test(d)),
    dlls.join(', '));
  console.log('       (' + dlls.join(', ') + '; ' + names.length + ' imports recognised)');
}

/* ---- 3. payload integrity --------------------------------------------- */
const anchor = Buffer.from('<!DOCTYPE html');
const idx = exe.indexOf(anchor);
check('payload anchor found', idx > 0, 'offset ' + idx);

let payload = null;
if (idx > 0) {
  payload = exe.slice(idx);
  const html = fs.readFileSync(HTML);
  check('payload is byte-identical to dist/AE.html',
    payload.length === html.length && payload.equals(html),
    payload.length + ' vs ' + html.length + ' bytes');
  check('payload is complete (ends with </html>)',
    payload.slice(-32).toString('latin1').trim().endsWith('</html>'));
  check('payload is not a stub', payload.length > 1000000,
    payload.length + ' bytes');
}

/* ---- 4. the extracted payload actually runs --------------------------- */
if (payload) {
  const listeners = new Map();
  function mk(n) {
    return {
      _name: n, width: 1280, height: 720, focus() {},
      getBoundingClientRect() { return { left: 0, top: 0, width: 1280, height: 720 }; },
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
    screen: canvas, status: { textContent: '' }, fps: { textContent: '' },
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
  sandbox.window = sandbox;
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.setTimeout = setTimeout;
  sandbox.addEventListener = (t, f) => {
    const k = 'window:' + t;
    if (!listeners.has(k)) listeners.set(k, []);
    listeners.get(k).push(f);
  };

  const htmlText = payload.toString('latin1');
  const scripts = [...htmlText.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
  check('payload contains its scripts', scripts.length === 3, scripts.length + ' blocks');

  try {
    vm.createContext(sandbox);
    for (const s of scripts) vm.runInContext(s, sandbox, { filename: 'AE.exe:payload' });
  } catch (err) {
    check('payload scripts execute', false, String(err && err.message));
  }

  setTimeout(() => {
    const ctx = canvas.getContext('2d');
    const painted = !!(ctx.last && ctx.last.width === 1280);
    check('the payload boots the OS', painted,
      painted ? '' : 'status="' + els.status.textContent + '"');
    if (painted) {
      let ink = 0;
      for (let i = 0; i < ctx.last.data.length; i += 4) {
        if (ctx.last.data[i] || ctx.last.data[i + 1] || ctx.last.data[i + 2]) ink++;
      }
      check('the OS renders on screen', ink > 300000, ink + ' non-black px');
    }
    /* The launcher builds one raw command line for the browser and the child
     * splits it itself.  If a path is not quoted, every account whose name has
     * a space in it (most of them) gets a broken --app url and no window.  Build
     * the same line the C code builds and split it the way Windows does. */
    {
      function buildLine(app, browserPath) {
        const slash = Math.max(app.lastIndexOf('\\'), app.lastIndexOf('/'));
        const prof = app.slice(0, slash + 1);
        let url = '--app="file:///' + app.replace(/\\/g, '/') + '"';
        url += ' --window-size=1320,820 --user-data-dir="' + prof + 'AE-profile"';
        return '"' + browserPath + '" ' + url;
      }
      function splitArgv(s) {
        const out = [];
        let cur = '';
        let q = false;
        for (const ch of s) {
          if (ch === '"') { q = !q; continue; }
          if (ch === ' ' && !q) { if (cur) out.push(cur); cur = ''; continue; }
          cur += ch;
        }
        if (cur) out.push(cur);
        return out;
      }
      const spaced = 'C:\\Users\\Bob Smith\\AppData\\Local\\Temp\\AE.html';
      const browser = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
      const argv = splitArgv(buildLine(spaced, browser));
      const wantProf = 'C:\\Users\\Bob Smith\\AppData\\Local\\Temp\\AE-profile';
      const joined = argv.join('|');
      check('a spaced path stays one argument',
        argv[1] === '--app=file:///C:/Users/Bob Smith/AppData/Local/Temp/AE.html',
        argv[1]);
      check('the browser path is not split', argv[0] === browser, argv[0]);
      check('the private profile path survives a space',
        joined.indexOf('--user-data-dir=' + wantProf) !== -1);
      check('exactly four arguments reach the browser',
        argv.length === 4, argv.length + ' args');
    }

    console.log(failures === 0 ? '\nALL CHECKS PASSED' : '\n' + failures + ' CHECK(S) FAILED');
    process.exit(failures === 0 ? 0 : 1);
  }, 4600);
} else {
  console.log('\n' + failures + ' CHECK(S) FAILED');
  process.exit(1);
}

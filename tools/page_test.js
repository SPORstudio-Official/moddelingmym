/* ==========================================================================
 *  dist/install.html test
 *
 *  The download page cannot be trusted to "look right": the thing that matters
 *  is whether clicking the button actually hands the browser a verified file,
 *  and whether every way it can go wrong refuses to save anything.  So this
 *  drives the page's own script in a fake DOM, with the network stubbed to four
 *  responses the real API can produce:
 *
 *    200 + file bytes   -> verified, saved as AE.exe
 *    404                -> explains the branch does not have it, saves nothing
 *    200 + JSON body    -> explains the API returned a description, saves nothing
 *    200 + tampered     -> checksum mismatch, saves nothing
 *
 *  Run: node tools/page_test.js
 * ======================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const vm = require('vm');

const ROOT = path.join(__dirname, '..');
const PAGE = path.join(ROOT, 'dist', 'install.html');
const EXE = path.join(ROOT, 'dist', 'AE.exe');

let failures = 0;
function check(name, cond, detail) {
  if (cond) console.log('  ok   ' + name);
  else { console.log('  FAIL ' + name + (detail ? '  -- ' + detail : '')); failures++; }
}

const html = fs.readFileSync(PAGE, 'utf8');
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
const file = fs.readFileSync(EXE);
const fileSha = crypto.createHash('sha256').update(file).digest('hex');

console.log('install page test\n');

/* ---- 1. the page pins the file that is actually in dist/ ---------------- */
{
  const pinned = /var SHA\s*=\s*'([0-9a-f]{64})'/.exec(html);
  const bytes = /var BYTES\s*=\s*(\d+)/.exec(html);
  check('page pins a SHA-256', !!pinned);
  check('the pinned hash is dist/AE.exe\'s',
    pinned && pinned[1] === fileSha,
    pinned ? pinned[1].slice(0, 16) + ' vs ' + fileSha.slice(0, 16) : '');
  check('the pinned byte count is dist/AE.exe\'s',
    bytes && Number(bytes[1]) === file.length,
    bytes ? bytes[1] + ' vs ' + file.length : '');
  /* Not "does the string appear anywhere" -- the comment above API_URL names
     the raw host while explaining why it is not used, which is a false
     positive. Check the URL the code actually builds instead. */
  {
    const api = /var API_URL\s*=\s*'([^']*)'/.exec(html);
    check('builds its request URL from the API', !!api && /^https:\/\/api\.github\.com\//.test(api[1]),
      api ? api[1] : 'API_URL not found');
    const base = /var OWNER = '([^']+)';[\s\S]*?var REPO\s*=\s*'([^']+)'/.exec(html);
    check('names the repository it downloads from',
      !!base && base[1] === 'SPORstudio-Official' && base[2] === 'moddelingmym',
      base ? base[1] + '/' + base[2] : '');
  }
  check('asks for the raw media type', html.includes('application/vnd.github.raw'));
  check('hashes in the browser', html.includes('crypto.subtle.digest'));
}

/* ---- 2. drive each outcome --------------------------------------------- */
function run(resp) {
  const els = {};
  function el(id) {
    if (!els[id]) {
      els[id] = {
        id, textContent: '', innerHTML: '', disabled: false, href: '', download: '',
        style: {},
        classList: {
          _s: new Set(),
          add(c) { this._s.add(c); },
          remove(c) { this._s.delete(c); },
          toggle(c, on) { if (on) this._s.add(c); else this._s.delete(c); },
          contains(c) { return this._s.has(c); }
        },
        addEventListener(t, f) { if (t === 'click') this.onclick = f; }
      };
    }
    return els[id];
  }

  let saved = null;
  const headers = {};

  function XHR() {
    this.open = () => {};
    this.setRequestHeader = (k, v) => { headers[k] = v; };
    this.getResponseHeader = (k) =>
      (k.toLowerCase() === 'content-type' ? resp.ct : null);
    this.send = () => {
      this.status = resp.status;
      /* Hand the body over as an ArrayBuffer, exactly as responseType does. */
      this.response = resp.body.buffer.slice(
        resp.body.byteOffset, resp.body.byteOffset + resp.body.byteLength);
      if (this.onload) this.onload();
    };
  }

  const sandbox = {
    window: { crypto: crypto.webcrypto },
    document: {
      getElementById: el,
      createElement: () => ({
        click() { saved = this.download; },
        style: {}
      }),
      body: { appendChild() {}, removeChild() {} }
    },
    XMLHttpRequest: XHR,
    Blob: class { constructor(parts) { this.size = parts[0].byteLength; } },
    URL: { createObjectURL: () => 'blob:ae', revokeObjectURL() {} },
    setTimeout: () => {}
  };

  const ctx = vm.createContext(sandbox);
  for (const s of scripts) vm.runInContext(s, ctx, { filename: 'install.html' });
  el('go').onclick();

  return new Promise((resolve) => {
    /* The page hashes asynchronously, so let the microtask queue drain. */
    setTimeout(() => resolve({
      saved, headers, el,
      failShown: el('step-fail').classList.contains('on'),
      okShown: el('result-box').classList.contains('on'),
      failMsg: el('fail-msg').innerHTML,
      resultTitle: el('result-title').textContent,
      launchShown: el('step-launching').classList.contains('on')
    }), 250);
  });
}

(async () => {
  console.log('\n-- a good download');
  {
    const r = await run({ status: 200, ct: 'application/vnd.github.raw', body: file });
    check('saved a file named AE.exe', r.saved === 'AE.exe', String(r.saved));
    check('said it was verified', /verified/i.test(r.resultTitle), r.resultTitle);
    check('told the user to open it', r.launchShown);
    check('did not show a failure', !r.failShown);
    check('requested the raw media type', r.headers.Accept === 'application/vnd.github.raw');
  }

  console.log('\n-- 404 (file not on that branch)');
  {
    const r = await run({
      status: 404, ct: 'application/json',
      body: Buffer.from('{"message":"Not Found"}')
    });
    check('saved nothing', r.saved === null);
    check('showed the failure panel', r.failShown);
    check('explained the branch problem', /not on that branch/i.test(r.failMsg));
  }

  console.log('\n-- 200 but a JSON description');
  {
    const r = await run({
      status: 200, ct: 'application/json; charset=utf-8',
      body: Buffer.from('{"content":"abc"}')
    });
    check('saved nothing', r.saved === null);
    check('showed the failure panel', r.failShown);
    check('explained the JSON body', /description/i.test(r.failMsg));
  }

  console.log('\n-- tampered bytes');
  {
    const r = await run({
      status: 200, ct: 'application/vnd.github.raw',
      body: Buffer.concat([file, Buffer.from('X')])
    });
    check('saved nothing', r.saved === null);
    check('showed the failure panel', r.failShown);
    check('reported a checksum mismatch', /mismatch/i.test(r.failMsg));
  }

  console.log(failures === 0 ? '\nALL CHECKS PASSED' : '\n' + failures + ' CHECK(S) FAILED');
  process.exit(failures === 0 ? 0 : 1);
})();

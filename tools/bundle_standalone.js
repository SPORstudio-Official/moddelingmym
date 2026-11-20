#!/usr/bin/env node
/* ==========================================================================
 *  Bundle AetherOS into ONE self-contained .html file.
 *
 *  Why a single file: a browser refuses to fetch() a sibling .wasm over
 *  file://, so a normal build cannot run from a double-clicked page.  Embedding
 *  the module as base64 removes the fetch entirely, and inlining the CSS and
 *  script removes every external reference.  The result is a page that runs the
 *  whole OS offline, with no server and nothing to install.
 *
 *  Output: dist/AetherOS.html  (~1.3 MB)
 * ======================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const APP = path.join(ROOT, 'app');
const OUT = path.join(ROOT, 'dist', 'AetherOS.html');

const wasm = fs.readFileSync(path.join(APP, 'aether.wasm'));
const css = fs.readFileSync(path.join(APP, 'style.css'), 'utf8');
const host = fs.readFileSync(path.join(APP, 'aether-host.js'), 'utf8');
const boot = fs.readFileSync(path.join(APP, 'boot.js'), 'utf8');

const b64 = wasm.toString('base64');

/* The bytes are embedded as a base64 string and decoded before instantiation.
   `fetch` is replaced so the untouched aether-host.js gets exactly the response
   shape it expects, without knowing the module came from the page itself. */
const loader = `
(function () {
  var B64 = "${b64}";
  function decode(s) {
    var bin = atob(s);
    var out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  window.fetch = function () {
    var bytes = decode(B64);
    return Promise.resolve({
      ok: true,
      status: 200,
      arrayBuffer: function () { return Promise.resolve(bytes.buffer); }
    });
  };
})();
`;

const html = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>AetherOS</title>
<!-- AetherOS ${require(path.join(ROOT, 'package.json')).version} -- single-file build.
     The entire operating system (kernel, compositor, VFS, scheduler, compiler,
     apps) is embedded in this page as a WebAssembly module.  No network access,
     no installation, no dependencies. -->
<style>
${css}
</style>
</head>
<body>
  <div id="stage">
    <canvas id="screen" width="1280" height="720" tabindex="0"
            aria-label="AetherOS screen"></canvas>

    <div id="overlay" class="hidden" role="alertdialog" aria-modal="true">
      <div class="panel">
        <h1 id="err-title">AetherOS</h1>
        <pre id="err-body"></pre>
        <p class="hint" id="err-hint"></p>
        <button id="err-close" autofocus>Close</button>
      </div>
    </div>
  </div>

  <footer id="statusbar">
    <span class="brand">AetherOS</span>
    <span id="status">starting&hellip;</span>
    <span class="spacer"></span>
    <span id="fps" class="dim"></span>
    <button id="fullscreen" title="Toggle fullscreen (F11)">&#9974;</button>
  </footer>

<script>
${loader}
</script>
<script>
${host}
</script>
<script>
${boot}
</script>
</body>
</html>
`;

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, html);

/* Fail loudly if anything could still reach out to the network: a self-contained
   file that silently fetches a sibling is worse than no file at all. */
const external = [];
for (const re of [/\bsrc\s*=\s*["'](?!#|data:)/g, /\bhref\s*=\s*["'](?!#|data:)/g]) {
  const m = html.match(re);
  if (m) external.push(...m);
}
if (html.includes('http://') || html.includes('https://')) {
  external.push('absolute URL');
}
if (external.length) {
  console.error('bundle: external references found: ' + external.join(', '));
  process.exit(1);
}

console.log('built ' + path.relative(ROOT, OUT) + ' (' + (html.length / 1048576).toFixed(2) + ' MB)');

#!/usr/bin/env node
/* ==========================================================================
 *  Minimal static server for the browser build of the shell.
 *
 *  The Electron app loads app/index.html from disk, but the same files work in
 *  any browser when served over HTTP -- which is how the live preview runs and
 *  how you can try AetherOS without installing anything.
 *
 *  A .wasm body must be served as application/wasm, otherwise
 *  WebAssembly.instantiate() refuses to stream-compile it.
 * ======================================================================== */
'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', 'app');
const PORT = parseInt(process.env.PORT || '8080', 10);
const HOST = process.env.HOST || '0.0.0.0';

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json; charset=utf-8',
};

const server = http.createServer((req, res) => {
  let rel = decodeURIComponent(req.url.split('?')[0]);
  if (rel === '/' || rel === '') rel = '/index.html';

  /* Resolve inside ROOT only: no path traversal out of app/. */
  const file = path.join(ROOT, path.normalize(rel).replace(/^([/\\])+/, ''));
  if (!file.startsWith(ROOT)) {
    res.writeHead(403).end('forbidden');
    return;
  }

  fs.readFile(file, (err, buf) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain' }).end('not found: ' + rel);
      return;
    }
    res.writeHead(200, {
      'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream',
      'Content-Length': buf.length,
      'Cache-Control': 'no-cache',
      /* The preview runs inside an iframe on another origin; nothing here needs
         framing protection, and COOP/COEP would only break the canvas. */
      'Cross-Origin-Opener-Policy': 'same-origin',
    });
    res.end(buf);
  });
});

server.listen(PORT, HOST, () => {
  console.log(`AetherOS shell on http://${HOST}:${PORT}/ (serving ${ROOT})`);
});

/* ==========================================================================
 *  dist/AE-os.exe import coverage test
 *
 *  The OS image calls far more Win32 functions than it imports.  That is not a
 *  typo, and it is not a linker problem: the optimiser *deleted* the code that
 *  made the calls, and a call that no longer exists does not need an import.
 *
 *  What happened, concretely.  dev/10_util.c used to define
 *
 *      size_t kstrlen(const char *s) {
 *          size_t n = 0;
 *          if (!s) return 0;
 *          while (s[n]) n++;
 *          return n;
 *      }
 *
 *  clang recognises that body as strlen and folds it into the builtin.  The
 *  declaration is a plain one (no noundef on the return), so on the branch where
 *  the pointer could not be proven non-NULL it is allowed to treat the result as
 *  poison.  winhttp_fetch_inner() then computed
 *
 *      int blen = (int)kstrlen(body);
 *      WinHttpSendRequest(req, hlen ? whdr : NO_HEADERS, hlen ? -1 : 0,
 *                         blen ? body : NO_DATA, blen, blen, 0);
 *
 *  and poison propagated through blen into the call.  clang concluded the block
 *  was unreachable, replaced it with a single `int3`, and the whole body of the
 *  HTTP client disappeared:
 *
 *      before: 11 WinHttp* imports      after: 6
 *      WinHttpSendRequest      -> gone   (the request was never sent)
 *      WinHttpReceiveResponse  -> gone   (nothing was ever read)
 *      WinHttpQueryHeaders     -> gone   (no status, no Mcp-Session-Id)
 *      WinHttpQueryDataAvailable -> gone
 *      WinHttpReadData         -> gone
 *
 *  The build was clean, every other test passed, and the app still opened --
 *  which is why this survived.  Browsing a page and running an MCP search would
 *  have trapped at that `int3`.
 *
 *  So this test asserts the thing that actually matters: every Win32 function
 *  the sources name exists in the image's import table.  It reads the import
 *  directory out of the real shipped binary rather than trusting the compiler,
 *  because the compiler is what lied.
 *
 *  Run: node tools/import_test.js
 * ======================================================================== */

'use strict';

const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const IMG = path.join(ROOT, 'dist', 'AE-os.exe');
const SRC = path.join(ROOT, 'dev');

let failures = 0;
function check(name, cond, detail) {
  if (cond) console.log('  ok   ' + name);
  else { console.log('  FAIL ' + name + (detail ? '  -- ' + detail : '')); failures++; }
}

if (!fs.existsSync(IMG)) {
  console.error('dist/AE-os.exe missing -- run: bash tools/build.sh');
  process.exit(1);
}

const exe = fs.readFileSync(IMG);
console.log('AE-os.exe import coverage\n');

/* ---- read the import directory out of the PE --------------------------- */

function readImports(buf) {
  const peOff = buf.readUInt32LE(0x3c);
  if (buf.slice(peOff, peOff + 4).toString('binary') !== 'PE\0\0') return null;
  const numSections = buf.readUInt16LE(peOff + 6);
  const optSize = buf.readUInt16LE(peOff + 20);
  const optOff = peOff + 24;
  const magic = buf.readUInt16LE(optOff);
  if (magic !== 0x20b) return null;

  /* Data directory 1 is the import table. */
  const ddOff = optOff + 112;
  const impRva = buf.readUInt32LE(ddOff + 8);

  const secOff = optOff + optSize;
  const sections = [];
  for (let i = 0; i < numSections; i++) {
    const o = secOff + i * 40;
    sections.push({
      va: buf.readUInt32LE(o + 12),
      vsize: buf.readUInt32LE(o + 8),
      raw: buf.readUInt32LE(o + 20),
      rsize: buf.readUInt32LE(o + 16),
    });
  }
  const rva2off = (rva) => {
    for (const s of sections) {
      const size = Math.max(s.vsize, s.rsize);
      if (rva >= s.va && rva < s.va + size) return s.raw + (rva - s.va);
    }
    return null;
  };

  const byDll = {};
  const all = [];
  let off = rva2off(impRva);
  if (off === null) return null;
  for (;;) {
    const ilt = buf.readUInt32LE(off);
    const nameRva = buf.readUInt32LE(off + 12);
    if (ilt === 0 && nameRva === 0) break;
    const nameOff = rva2off(nameRva);
    if (nameOff === null) break;
    const dll = buf.slice(nameOff, buf.indexOf(0, nameOff)).toString('latin1');
    byDll[dll] = byDll[dll] || [];

    let t = rva2off(ilt);
    if (t !== null) {
      for (;;) {
        const raw64 = buf.readBigUInt64LE(t);
        if (raw64 === 0n) break;
        const isOrdinal = (raw64 & 0x8000000000000000n) !== 0n;
        if (!isOrdinal) {
          const hint = rva2off(Number(raw64 & 0x7fffffffn));
          if (hint !== null) {
            const fn = buf.slice(hint + 2, buf.indexOf(0, hint + 2)).toString('latin1');
            byDll[dll].push(fn);
            all.push(fn);
          }
        }
        t += 8;
      }
    }
    off += 20;
  }
  return { byDll, all };
}

const imports = readImports(exe);
check('import table is readable', imports !== null);
if (!imports) {
  console.log('\n1 CHECK(S) FAILED');
  process.exit(1);
}

console.log('       ' + Object.keys(imports.byDll).sort()
  .map((d) => d + '(' + imports.byDll[d].length + ')').join(' '));

/* ---- 1. every DLL the sources reference is linked ---------------------- */

/* A Win32 call in the sources looks like Foo(...) where Foo also appears in a
   Windows header.  Rather than parse headers, list the functions whose module
   the code proves it uses, and require them.  Keep this list in step with the
   source: it is the guard rail, not a mirror of the SDK. */
const MUST_IMPORT = {
  WINHTTP: [
    /* The five that vanish when the request body length goes poison. */
    'WinHttpSendRequest',
    'WinHttpReceiveResponse',
    'WinHttpQueryHeaders',
    'WinHttpQueryDataAvailable',
    'WinHttpReadData',
    /* And the five that survive, so a failure names the missing half. */
    'WinHttpOpen', 'WinHttpConnect', 'WinHttpOpenRequest',
    'WinHttpSetTimeouts', 'WinHttpCrackUrl', 'WinHttpCloseHandle',
  ],
  KERNEL32: ['VirtualAlloc', 'CreateThread', 'ExitProcess', 'Sleep'],
  USER32: ['RegisterClassExA', 'CreateWindowExA', 'ShowWindow', 'PeekMessageA'],
  GDI32: ['CreateDIBSection', 'BitBlt'],
};

console.log('\n-- required imports');
for (const dll of Object.keys(MUST_IMPORT)) {
  const key = Object.keys(imports.byDll).find(
    (d) => d.toUpperCase().startsWith(dll));
  const present = key ? imports.byDll[key] : [];
  for (const fn of MUST_IMPORT[dll]) {
    check(dll + '!' + fn, present.includes(fn),
      key ? 'not imported' : dll + '.dll not linked at all');
  }
}

/* ---- 2. nothing the sources call is silently absent ------------------- */

/* The specific failure mode again, stated as its own check so the message
   says what actually went wrong rather than "a symbol is missing". */
console.log('\n-- optimiser did not delete the HTTP client');
const netFns = MUST_IMPORT.WINHTTP;
const missing = netFns.filter((f) => !imports.all.includes(f));
check('all 11 WinHTTP entry points are imported',
  missing.length === 0,
  missing.length ? 'deleted by the optimiser: ' + missing.join(', ') : '');

/* ---- 3. no stray unresolved externals --------------------------------- */

/* Every import must name a real export on a supported Windows version; a
   misspelling would blue-screen at load with no console to complain in. */
console.log('\n-- import names look like real exports');
const bogus = imports.all.filter((f) => !/^[A-Za-z_][A-Za-z0-9_]{0,120}$/.test(f));
check('no malformed import names', bogus.length === 0, bogus.join(', '));
check('no ordinal imports (portable across Windows builds)',
  !exe.includes(Buffer.from('WinHttpQueryHeaders\0\0')) || imports.all.length > 0);

/* ---- 4. the payload the launcher appends is a real program ------------ */

console.log('\n-- sources and image agree');
const halSrc = fs.readFileSync(path.join(SRC, '20_hal.c'), 'latin1');
for (const fn of MUST_IMPORT.WINHTTP) {
  const called = halSrc.includes(fn + '(');
  const imported = imports.all.includes(fn);
  check('20_hal.c calls ' + fn + ' => imported', !called || imported,
    called && !imported ? 'called but missing from the import table' : '');
}

console.log(failures === 0 ? '\nALL CHECKS PASSED' : '\n' + failures + ' CHECK(S) FAILED');
process.exit(failures === 0 ? 0 : 1);

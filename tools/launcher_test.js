/* ==========================================================================
 *  Launcher payload scanner test
 *
 *  This exists because of a real shipped bug.  The launcher finds its OS image
 *  by scanning for a marker and then looking for a PE header after it, and its
 *  e_lfanew test was wrong: it read the 32-bit offset as two single bytes, so it
 *  rejected zig's perfectly valid `e_lfanew = 0x78` and reported "this file
 *  seems to be incomplete" for a build that was fine.
 *
 *  Nothing caught it, because build_exe.sh and exe_test.js each re-implement
 *  the same scan -- correctly -- and neither ever ran the launcher's copy.  So
 *  this test does the one thing that would have: it compiles the launcher's own
 *  `launcher_find_payload()` for the host with a tiny harness, runs it over the
 *  real dist/AE.exe, and checks it finds exactly the executable that was
 *  appended.
 *
 *  Run: node tools/launcher_test.js
 *  Needs a host C compiler (cc/gcc/clang); skips with a clear message without.
 * ======================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFileSync, spawnSync } = require('child_process');

const ROOT = path.join(__dirname, '..');
const LAUNCHER = path.join(ROOT, 'tools', 'launcher', 'launcher.c');
const EXE = path.join(ROOT, 'dist', 'AE.exe');
const INNER = path.join(ROOT, 'dist', 'AE-os.exe');
const TMP = fs.mkdtempSync(path.join(require('os').tmpdir(), 'ae-launcher-test-'));

let failures = 0;
function check(name, cond, detail) {
  if (cond) console.log('  ok   ' + name);
  else { console.log('  FAIL ' + name + (detail ? '  -- ' + detail : '')); failures++; }
}

console.log('launcher scanner test\n');

/* ---- 0. find a host compiler ------------------------------------------- */
const CC = ['cc', 'gcc', 'clang'].find((c) =>
  spawnSync(c, ['--version'], { stdio: 'ignore' }).status === 0);

if (!CC) {
  console.log('  skip no host C compiler (cc/gcc/clang) found');
  console.log('\nSKIPPED');
  process.exit(0);
}

/* ---- 1. build the harness ----------------------------------------------
 * The launcher is a Win32 freestanding program, so it cannot be linked whole.
 * Only the two functions under test are needed, and they depend on nothing but
 * the file's own string helpers, which the harness supplies. */
const harness = `
int main(int argc, char **argv) {
    FILE *f;
    long size;
    unsigned char *buf;
    const unsigned char *found = NULL;
    unsigned long long found_len = 0;

    if (argc < 2) return 2;
    f = fopen(argv[1], "rb");
    if (!f) return 3;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)size + 16);
    if (!buf) return 4;
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) return 5;
    fclose(f);

    if (!launcher_find_payload(buf, (unsigned long long)size, &found, &found_len)) {
        printf("FOUND none\\n");
        return 1;
    }
    printf("FOUND %llu %llu\\n",
           (unsigned long long)(found - buf), found_len);

    /* Also dump the payload so the test can compare bytes, not just offsets. */
    f = fopen(argv[2], "wb");
    if (f) { fwrite(found, 1, (size_t)found_len, f); fclose(f); }
    return 0;
}
`;

/* The launcher includes <windows.h> and uses Win32 types, so the harness keeps
   only the scanner section: everything from the marker down to the end of
   launcher_find_payload().  Extracting it here (rather than #ifdef-ing the
   production file) keeps the shipped source readable. */
const src = fs.readFileSync(LAUNCHER, 'utf8');

function slice(startMarker, endMarker) {
  const a = src.indexOf(startMarker);
  if (a < 0) throw new Error('marker not found: ' + startMarker);
  const b = src.indexOf(endMarker, a);
  if (b < 0) throw new Error('end marker not found: ' + endMarker);
  return src.slice(a, b);
}

/* pe_image_at() ends where the scanner comment begins, and the scanner ends at
   payload_read(); slicing both to payload_read() duplicated the scanner. */
const peImageAt = slice('static int pe_image_at(', '/* Find the appended image inside a buffer');
const finder = slice('int launcher_find_payload(', 'static payload_t payload_read(');

check('found pe_image_at() in launcher.c', peImageAt.length > 100);
check('found launcher_find_payload() in launcher.c', finder.length > 100);
check('the scanner is exposed for testing (not static)',
  /^int launcher_find_payload\(/m.test(finder));

/* Order matters: the scanner references PAYLOAD_MAGIC and MAGIC_LEN, so those
   come first, then the two functions under test, then main(). */
const unit = [
  '#include <stdio.h>',
  '#include <stdlib.h>',
  '#include <string.h>',
  '',
  '/* The launcher marks this array with a section attribute for the scanner to',
  '   find; on the host a plain array with the same bytes is equivalent. */',
  'static const char PAYLOAD_MAGIC[] = "\\nAETHEROS-PAYLOAD-V1\\n";',
  '#define MAGIC_LEN (sizeof(PAYLOAD_MAGIC) - 1)',
  '',
  peImageAt,
  finder,
  harness
].join('\n');

const unitPath = path.join(TMP, 'scanner.c');
const binPath = path.join(TMP, 'scanner');
fs.writeFileSync(unitPath, unit);

let built = false;
try {
  execFileSync(CC, ['-O1', '-std=c11', '-Wall', '-Wextra', '-o', binPath, unitPath],
    { stdio: 'pipe' });
  built = true;
} catch (e) {
  const msg = (e.stderr || e.stdout || '').toString().split('\n').slice(0, 6).join('\n');
  check('the scanner compiles standalone', false, msg);
}

if (!built) {
  console.log('\n' + failures + ' CHECK(S) FAILED');
  process.exit(1);
}
console.log('  ok   the scanner compiles standalone');

/* ---- 2. run it over the shipped executable ----------------------------- */
const dumped = path.join(TMP, 'payload.bin');
/* spawnSync, not execFileSync: the whole point is a readable result when the
   scanner fails, and execFileSync throws away the message in a stack trace. */
const run = spawnSync(binPath, [EXE, dumped], { stdio: 'pipe' });
const out = (run.stdout || '').toString().trim();
const [tag, offsetStr, lenStr] = out.split(' ');

check('it finds a payload in dist/AE.exe', tag === 'FOUND',
  'scanner said: ' + out);
if (tag === 'FOUND') {
  const offset = Number(offsetStr);
  const len = Number(lenStr);
  const exeSize = fs.statSync(EXE).size;

  check('the payload starts after the launcher', offset > 0, String(offset));
  check('the payload runs to end of file', offset + len === exeSize,
    offset + ' + ' + len + ' vs ' + exeSize);
  check('the payload is a plausible size', len > 300000, String(len));

  /* Only read the dump when the scanner actually wrote one -- with a broken
     * scanner this is exactly where the failure must be reported, not thrown. */
  if (!fs.existsSync(dumped)) {
    check('the scanner dumped the payload it found', false, 'no file written');
  } else if (fs.existsSync(INNER)) {
    const inner = fs.readFileSync(INNER);
    const got = fs.readFileSync(dumped);
    check('the payload is byte-identical to dist/AE-os.exe',
      got.length === inner.length && got.equals(inner),
      got.length + ' vs ' + inner.length);
    check('the payload really is a PE image',
      got.slice(0, 2).toString('latin1') === 'MZ');
    const peOff = got.readUInt32LE(0x3c);
    check('its e_lfanew points at a PE signature',
      got.slice(peOff, peOff + 4).toString('binary') === 'PE\0\0',
      'e_lfanew=' + peOff);
    /* The exact value that broke the old byte-range test. */
    console.log('       (e_lfanew = 0x' + peOff.toString(16) + ')');
  }
}

/* ---- 3. it must not invent a payload ----------------------------------- */
{
  const junk = path.join(TMP, 'junk.exe');
  fs.writeFileSync(junk, Buffer.concat([
    fs.readFileSync(EXE, { encoding: null }).slice(0, 4096),
    Buffer.from('\nAETHEROS-PAYLOAD-V1\n', 'latin1'),
    Buffer.from('not a PE file at all, just text', 'latin1')
  ]));
  const r = spawnSync(binPath, [junk, path.join(TMP, 'no.bin')], { stdio: 'pipe' });
  check('a marker with no PE after it finds nothing', r.status === 1,
    'exit ' + r.status + ': ' + (r.stdout || '').toString().trim());
}

/* ---- 4. a truncated payload is still located, not rejected -------------
 * A half-written file is a different failure from a missing one; the launcher
 * checks the length afterwards so it can say which. */
{
  const full = fs.readFileSync(EXE);
  const cut = path.join(TMP, 'cut.exe');
  fs.writeFileSync(cut, full.slice(0, full.length - 5000));
  const r = spawnSync(binPath, [cut, path.join(TMP, 'cut.bin')], { stdio: 'pipe' });
  check('a truncated file still yields a payload', r.status === 0,
    (r.stdout || '').toString().trim());
}

fs.rmSync(TMP, { recursive: true, force: true });
console.log(failures === 0 ? '\nALL CHECKS PASSED' : '\n' + failures + ' CHECK(S) FAILED');
process.exit(failures === 0 ? 0 : 1);

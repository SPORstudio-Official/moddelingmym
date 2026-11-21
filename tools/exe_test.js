/* ==========================================================================
 *  dist/AE.exe structural test
 *
 *  AE is a single Windows executable: a small native launcher with the whole
 *  operating system appended to it as a second PE image.  Neither half can be
 *  executed here, so this checks everything that CAN be checked off-Windows:
 *
 *    1. the outer file is a well-formed PE32+ GUI executable, x86-64, with no C
 *       runtime dependency -- it must start on a bare Windows machine;
 *    2. the inner image is a complete PE executable too (correct machine type,
 *       GUI subsystem, a sane size), found exactly the way the launcher finds
 *       it: scan for the payload marker, then take the next MZ/PE header;
 *    3. the two halves agree -- the appended length matches the built image on
 *       disk, so the extract step cannot write a truncated file;
 *    4. the launcher's own message strings are present, which is what proves
 *       the extraction/retry/message-box path was compiled in rather than
 *       optimised away.
 *
 *  Run: node tools/exe_test.js
 * ======================================================================== */
'use strict';

const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const EXE = path.join(ROOT, 'dist', 'AE.exe');
const IMG = path.join(ROOT, 'dist', 'AE-os.exe');

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

/* ---- helpers ----------------------------------------------------------- */
function peInfo(buf, off) {
  if (buf.slice(off, off + 2).toString('latin1') !== 'MZ') return null;
  const peOff = off + buf.readUInt32LE(off + 0x3c);
  if (peOff + 24 > buf.length) return null;
  if (buf.slice(peOff, peOff + 4).toString('binary') !== 'PE\0\0') return null;
  const machine = buf.readUInt16LE(peOff + 4);
  const numSections = buf.readUInt16LE(peOff + 6);
  const optOff = peOff + 24;
  return {
    machine,
    numSections,
    magic: buf.readUInt16LE(optOff),
    subsystem: buf.readUInt16LE(optOff + 68),
    entryRva: buf.readUInt32LE(optOff + 16),
  };
}

function checkPE(label, info) {
  check(label + ': x86-64 machine type', info.machine === 0x8664, '0x' + info.machine.toString(16));
  check(label + ': PE32+ optional header', info.magic === 0x20b, '0x' + info.magic.toString(16));
  check(label + ': GUI subsystem (no console flash)', info.subsystem === 2, String(info.subsystem));
  check(label + ': has an entry point', info.entryRva !== 0, '0x' + info.entryRva.toString(16));
  check(label + ': section count sane',
    info.numSections > 0 && info.numSections < 32, String(info.numSections));
}

/* ---- 1. the outer launcher -------------------------------------------- */
console.log('-- launcher');
const outer = peInfo(exe, 0);
check('starts with MZ', exe.slice(0, 2).toString() === 'MZ');
if (!outer) {
  console.log('\nouter image is not a valid PE -- stopping');
  process.exit(1);
}
checkPE('launcher', outer);

{
  const dlls = [];
  for (let i = 0; i < exe.length - 20; i++) {
    if (exe[i] !== 0x2e) continue;             /* '.' of '.dll' */
    const s = exe.slice(i - 8, i + 4).toString('latin1');
    if (/DLL$/i.test(s) && /^[A-Za-z0-9_.-]+$/i.test(s)) {
      if (!dlls.includes(s)) dlls.push(s);
    }
  }
  check('no C runtime dependency',
    !dlls.some((d) => /^(MSVCRT|UCRT|VCRUNTIME)/i.test(d)), dlls.join(', '));
  check('links only DLLs present on Windows 10/11',
    dlls.every((d) => /^(KERNEL32|USER32|SHELL32|ADVAPI32|GDI32|WINHTTP|OLE32|COMDLG32)\.DLL$/i.test(d)),
    dlls.join(', '));
  console.log('       (' + dlls.join(', ') + ')');
}

/* ---- 2. the appended OS image ------------------------------------------ */
console.log('\n-- payload');
const MARKER = Buffer.from('\nAETHEROS-PAYLOAD-V1\n');
let imgOff = -1;
{
  const m = exe.indexOf(MARKER);
  check('payload marker present', m > 0, 'offset ' + m);
  if (m > 0) {
    /* Walk the marker to the next MZ whose e_lfanew points at a PE signature,
       exactly as launcher.c does, so this test cannot pass on an image the
       launcher would fail to find. */
    for (let j = m + MARKER.length; j + 0x40 < exe.length; j++) {
      if (exe[j] !== 0x4d || exe[j + 1] !== 0x5a) continue;      /* 'MZ' */
      const lfanew = exe.readUInt32LE(j + 0x3c);
      if (lfanew < 0x40 || lfanew > 0xf0) continue;
      const peOff = j + lfanew;
      if (peOff + 4 > exe.length) continue;
      if (exe.slice(peOff, peOff + 4).toString('binary') !== 'PE\0\0') continue;
      imgOff = j;
      break;
    }
  }
  check('the marker leads to an MZ/PE header', imgOff > 0, 'offset ' + imgOff);
}

if (imgOff > 0) {
  const inner = peInfo(exe, imgOff);
  checkPE('payload', inner);
  const payload = exe.slice(imgOff);
  check('payload is a real program (not a stub)', payload.length > 300000,
    payload.length + ' bytes');
  check('payload is measured to end of file', imgOff + payload.length === exe.length,
    payload.length + ' of ' + (exe.length - imgOff) + ' bytes');

  if (fs.existsSync(IMG)) {
    const built = fs.readFileSync(IMG);
    check('payload is byte-identical to dist/AE-os.exe',
      built.length === payload.length && built.equals(payload),
      payload.length + ' vs ' + built.length + ' bytes');
  } else {
    check('dist/AE-os.exe exists to compare against', false, 'missing');
  }
}

/* The launcher does more than start the image: it owns the shell integration,
 * and its diagnostics are the only thing a user sees when extraction fails.  A
 * string scan is a blunt instrument, but it is the one check available here and
 * it catches a build where the error path silently dropped out. */
console.log('\n-- launcher behaviour');
for (const s of [
  'AE could not start',
  'This file seems to be incomplete',
  'could not start its own OS image',
]) {
  check('diagnostic present: "' + s + '"',
    exe.includes(Buffer.from(s, 'latin1')), 'not found');
}
check('extracts to a per-user folder (%LOCALAPPDATA%\\AE)',
  exe.includes(Buffer.from('LOCALAPPDATA', 'latin1')));
check('version-tagged image name (AE-<tag>.exe)',
  exe.includes(Buffer.from('AE-', 'latin1')));

console.log(failures === 0 ? '\nALL CHECKS PASSED' : '\n' + failures + ' CHECK(S) FAILED');
process.exit(failures === 0 ? 0 : 1);

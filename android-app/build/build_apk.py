#!/usr/bin/env python3
"""
Build and sign the Code Reviewer APK without the Android SDK.

No Android SDK, aapt2, d8 or javac is required. This script:
  1. writes a binary AndroidManifest.xml (AXML),
  2. generates classes.dex directly (one Activity hosting a WebView that loads
     the bundled assets/index.html - the analyzer runs inside it, offline),
  3. packages the bundled web assets from app/src/main/assets,
  4. signs the APK with a JAR (v1) signature using openssl. v1 is accepted on
     every Android version for apps with minSdkVersion 21.

Usage:  python3 android-app/build/build_apk.py
Output: android-app/release/CodeReviewer.apk
"""

import base64
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.abspath(os.path.join(HERE, '..'))
ASSETS_DIR = os.path.join(APP_DIR, 'app', 'src', 'main', 'assets')
RELEASE_DIR = os.path.join(APP_DIR, 'release')
SIGNING_DIR = os.path.join(APP_DIR, 'signing')

PACKAGE = 'com.sporstudio.codereviewer'
ACTIVITY = PACKAGE + '.MainActivity'
VERSION_CODE = 1
VERSION_NAME = '1.0.0'
MIN_SDK = 21
TARGET_SDK = 30
ASSET_URL = 'file:///android_asset/index.html'
APK_NAME = 'CodeReviewer.apk'


# ---------------------------------------------------------------------------
# Binary XML (AndroidManifest.xml)
# ---------------------------------------------------------------------------
ANDROID_NS = 'http://schemas.android.com/apk/res/android'
# Framework attribute resource IDs (from AOSP attrs_manifest.xml).
ATTR_IDS = {
    'label': 0x01010001,
    'icon': 0x01010002,
    'name': 0x01010003,
    'exported': 0x01010010,
    'minSdkVersion': 0x0101020C,
    'versionCode': 0x0101021B,
    'versionName': 0x0101021C,
    'windowSoftInputMode': 0x0101022B,
    'targetSdkVersion': 0x01010270,
    'allowBackup': 0x01010280,
}

TYPE_STRING = 0x03
TYPE_INT_DEC = 0x10
TYPE_INT_BOOLEAN = 0x12
NO_INDEX = 0xFFFFFFFF
SOFT_INPUT_ADJUST_RESIZE = 0x10


class StringPool:
    def __init__(self):
        self.strings = []
        self.index = {}

    def add(self, s):
        if s not in self.index:
            self.index[s] = len(self.strings)
            self.strings.append(s)
        return self.index[s]

    def encode(self):
        data = b''
        offsets = []
        for s in self.strings:
            offsets.append(len(data))
            data += struct.pack('<H', len(s)) + s.encode('utf-16-le') + b'\x00\x00'
        while len(data) % 4:
            data += b'\x00'
        header_size = 28
        strings_start = header_size + 4 * len(self.strings)
        size = strings_start + len(data)
        chunk = struct.pack('<HHIIIIII', 0x0001, header_size, size, len(self.strings), 0, 0,
                            strings_start, 0)
        chunk += b''.join(struct.pack('<I', o) for o in offsets)
        return chunk + data


def build_manifest():
    """Return the binary AndroidManifest.xml for the app."""
    pool = StringPool()
    id_names = list(ATTR_IDS.keys())
    for name in id_names:          # first N pool entries map 1:1 to resource IDs
        pool.add(name)
    ns_prefix = pool.add('android')
    ns_uri = pool.add(ANDROID_NS)

    # Element tree: (tag, [attributes], [children]).
    # Attribute: (kind, name, value, namespaced) with kind in str/int/bool.
    A = lambda kind, name, value, ns=True: (kind, name, value, ns)
    tree = ('manifest', [
        A('str', 'package', PACKAGE, False),
        A('int', 'versionCode', VERSION_CODE),
        A('str', 'versionName', VERSION_NAME),
    ], [
        ('uses-sdk', [A('int', 'minSdkVersion', MIN_SDK),
                      A('int', 'targetSdkVersion', TARGET_SDK)], []),
        ('application', [A('str', 'label', 'Code Reviewer'),
                         A('bool', 'allowBackup', False)], [
            ('activity', [A('str', 'name', ACTIVITY),
                          A('bool', 'exported', True),
                          A('int', 'windowSoftInputMode', SOFT_INPUT_ADJUST_RESIZE)], [
                ('intent-filter', [], [
                    ('action', [A('str', 'name', 'android.intent.action.MAIN')], []),
                    ('category', [A('str', 'name', 'android.intent.category.LAUNCHER')], []),
                ]),
            ]),
        ]),
    ])

    # Pre-register all strings so indices are stable before encoding.
    def collect(node):
        tag, attrs, children = node
        pool.add(tag)
        for kind, name, value, _ in attrs:
            pool.add(name)
            if kind == 'str':
                pool.add(value)
        for c in children:
            collect(c)
    collect(tree)

    line = 1
    body = struct.pack('<HHIIIII', 0x0100, 0x10, 0x18, line, NO_INDEX, ns_prefix, ns_uri)

    def emit(node):
        tag, attrs, children = node
        n = len(attrs)
        body_parts = struct.pack('<HHI', 0x0102, 0x10, 0x24 + 20 * n)
        body_parts += struct.pack('<II', line, NO_INDEX)
        body_parts += struct.pack('<II', NO_INDEX, pool.add(tag))
        body_parts += struct.pack('<HHHHHH', 0x14, 0x14, n, 0, 0, 0)
        for kind, name, value, namespaced in attrs:
            ns_idx = ns_uri if namespaced else NO_INDEX
            name_idx = pool.add(name)
            if kind == 'str':
                vidx = pool.add(value)
                body_parts += struct.pack('<IIIHBBI', ns_idx, name_idx, vidx, 8, 0, TYPE_STRING, vidx)
            elif kind == 'int':
                body_parts += struct.pack('<IIIHBBI', ns_idx, name_idx, NO_INDEX, 8, 0,
                                          TYPE_INT_DEC, value & 0xFFFFFFFF)
            else:
                body_parts += struct.pack('<IIIHBBI', ns_idx, name_idx, NO_INDEX, 8, 0,
                                          TYPE_INT_BOOLEAN, 0xFFFFFFFF if value else 0)
        parts = [body_parts]
        for child in children:
            parts.append(emit(child))
        parts.append(struct.pack('<HHIIIII', 0x0103, 0x10, 0x18, line, NO_INDEX, NO_INDEX, pool.add(tag)))
        return b''.join(parts)

    body += emit(tree)
    body += struct.pack('<HHIIIII', 0x0101, 0x10, 0x18, line, NO_INDEX, ns_prefix, ns_uri)

    res_map = struct.pack('<HHI', 0x0180, 8, 8 + 4 * len(id_names))
    res_map += b''.join(struct.pack('<I', ATTR_IDS[n]) for n in id_names)

    doc_body = pool.encode() + res_map + body
    return struct.pack('<HHI', 0x0003, 8, 8 + len(doc_body)) + doc_body


# ---------------------------------------------------------------------------
# DEX (classes.dex) - generated directly, no d8/javac needed.
#
#   public class com.sporstudio.codereviewer.MainActivity extends android.app.Activity {
#     public MainActivity() { super(); }
#     protected void onCreate(Bundle b) {
#       super.onCreate(b);
#       WebView w = new WebView(this);
#       WebSettings s = w.getSettings();
#       s.setJavaScriptEnabled(true);
#       s.setDomStorageEnabled(true);
#       w.loadUrl("file:///android_asset/index.html");
#       setContentView(w);
#     }
#   }
# ---------------------------------------------------------------------------
DEX_HEADER_SIZE = 0x70


def uleb128(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def adler32(data):
    a, b = 1, 0
    for i in range(0, len(data), 5552):
        for byte in data[i:i + 5552]:
            a += byte
            b += a
        a %= 65521
        b %= 65521
    return (b << 16) | a


def build_dex():
    def desc(cls):
        return 'L' + cls.replace('.', '/') + ';'

    main_desc = desc(ACTIVITY)
    ACT = desc('android.app.Activity')
    CTX = desc('android.content.Context')
    BUNDLE = desc('android.os.Bundle')
    VIEW = desc('android.view.View')
    WEBVIEW = desc('android.webkit.WebView')
    WEBSETTINGS = desc('android.webkit.WebSettings')
    STRING = desc('java.lang.String')

    # proto key -> (return type, parameter types)
    proto_defs = {
        'void': ('V', []),
        'onCreate': ('V', [BUNDLE]),
        'ctx': ('V', [CTX]),
        'settings': (WEBSETTINGS, []),
        'bool': ('V', ['Z']),
        'str': ('V', [STRING]),
        'view': ('V', [VIEW]),
    }

    def shorty(ret, params):
        def c(t):
            return t if t in ('V', 'Z') else 'L'
        return c(ret) + ''.join(c(p) for p in params)

    # (class, name, proto key)
    methods_def = [
        (ACT, '<init>', 'void'),
        (ACT, 'onCreate', 'onCreate'),
        (ACT, 'setContentView', 'view'),
        (WEBVIEW, '<init>', 'ctx'),
        (WEBVIEW, 'getSettings', 'settings'),
        (WEBVIEW, 'loadUrl', 'str'),
        (WEBSETTINGS, 'setDomStorageEnabled', 'bool'),
        (WEBSETTINGS, 'setJavaScriptEnabled', 'bool'),
        (main_desc, '<init>', 'void'),
        (main_desc, 'onCreate', 'onCreate'),
    ]

    # --- sorted tables -------------------------------------------------------
    raw = set(['<init>', 'onCreate', 'setContentView', 'getSettings', 'loadUrl',
               'setDomStorageEnabled', 'setJavaScriptEnabled', ASSET_URL])
    all_types = {main_desc, ACT, CTX, BUNDLE, VIEW, WEBVIEW, WEBSETTINGS, STRING, 'V', 'Z'}
    raw |= all_types
    for ret, params in proto_defs.values():
        raw.add(shorty(ret, params))
    strings = sorted(raw)
    sidx = {s: i for i, s in enumerate(strings)}

    types = sorted(all_types, key=lambda t: sidx[t])
    tidx = {t: i for i, t in enumerate(types)}

    protos = sorted(proto_defs.items(),
                    key=lambda kv: (tidx[kv[1][0]], [tidx[p] for p in kv[1][1]]))
    pidx = {k: i for i, (k, _) in enumerate(protos)}

    methods = sorted(methods_def, key=lambda m: (tidx[m[0]], sidx[m[1]], pidx[m[2]]))
    midx = {m: i for i, m in enumerate(methods)}

    # --- layout of id tables -------------------------------------------------
    off_strings = DEX_HEADER_SIZE
    off_types = off_strings + 4 * len(strings)
    off_protos = off_types + 4 * len(types)
    off_methods = off_protos + 12 * len(protos)     # no field ids
    off_classdefs = off_methods + 8 * len(methods)
    off_data = off_classdefs + 32

    data = bytearray()
    map_entries = []

    def pos():
        return off_data + len(data)

    def align(n):
        while len(data) % n:
            data.append(0)

    # type lists (proto parameters)
    type_list_off = {}
    first_type_list = None
    for key, (ret, params) in protos:
        if params:
            align(4)
            type_list_off[key] = pos()
            if first_type_list is None:
                first_type_list = pos()
            data += struct.pack('<I', len(params))
            for p in params:
                data += struct.pack('<H', tidx[p])
    if type_list_off:
        map_entries.append((0x1001, len(type_list_off), first_type_list))

    # string data
    string_data_off = []
    for s in strings:
        string_data_off.append(pos())
        data += uleb128(len(s)) + s.encode('ascii') + b'\x00'
    map_entries.append((0x2002, len(strings), string_data_off[0]))

    def unit(*vals):
        return b''.join(struct.pack('<H', v & 0xFFFF) for v in vals)

    def invoke(opcode, mkey, regs):
        r = list(regs) + [0] * (4 - len(regs))
        return unit(opcode | (len(regs) << 12), midx[mkey],
                    r[0] | (r[1] << 4) | (r[2] << 8) | (r[3] << 12))

    RETURN_VOID = unit(0x000E)
    INVOKE_VIRTUAL, INVOKE_SUPER, INVOKE_DIRECT = 0x6E, 0x6F, 0x70

    def code_item(registers, ins, outs, insns):
        align(4)
        off = pos()
        data.extend(struct.pack('<HHHHII', registers, ins, outs, 0, 0, len(insns) // 2))
        data.extend(insns)
        return off

    init_code = invoke(INVOKE_DIRECT, (ACT, '<init>', 'void'), [0]) + RETURN_VOID
    # registers: v0-v3 locals, v4 = this, v5 = savedInstanceState
    oncreate_code = b''.join([
        invoke(INVOKE_SUPER, (ACT, 'onCreate', 'onCreate'), [4, 5]),
        unit(0x22 | (0 << 8), tidx[WEBVIEW]),                                  # new-instance v0
        invoke(INVOKE_DIRECT, (WEBVIEW, '<init>', 'ctx'), [0, 4]),              # WebView(this)
        invoke(INVOKE_VIRTUAL, (WEBVIEW, 'getSettings', 'settings'), [0]),
        unit(0x0C | (1 << 8)),                                                  # move-result-object v1
        unit(0x12 | (2 << 8) | (1 << 12)),                                      # const/4 v2, #1
        invoke(INVOKE_VIRTUAL, (WEBSETTINGS, 'setJavaScriptEnabled', 'bool'), [1, 2]),
        invoke(INVOKE_VIRTUAL, (WEBSETTINGS, 'setDomStorageEnabled', 'bool'), [1, 2]),
        unit(0x1A | (3 << 8), sidx[ASSET_URL]),                                 # const-string v3
        invoke(INVOKE_VIRTUAL, (WEBVIEW, 'loadUrl', 'str'), [0, 3]),
        invoke(INVOKE_VIRTUAL, (ACT, 'setContentView', 'view'), [4, 0]),
        RETURN_VOID,
    ])
    init_off = code_item(1, 1, 1, init_code)
    oncreate_off = code_item(6, 2, 2, oncreate_code)
    map_entries.append((0x2001, 2, init_off))

    align(4)
    class_data_off = pos()
    init_idx = midx[(main_desc, '<init>', 'void')]
    oncreate_idx = midx[(main_desc, 'onCreate', 'onCreate')]
    # class_data_item: no fields; 1 direct method (<init>), 1 virtual method (onCreate)
    data += uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1)
    data += uleb128(init_idx) + uleb128(0x0001 | 0x10000) + uleb128(init_off)      # public ctor
    data += uleb128(oncreate_idx - init_idx) + uleb128(0x0004) + uleb128(oncreate_off)  # protected
    map_entries.append((0x2000, 1, class_data_off))

    align(4)
    map_off = pos()
    ordered = [
        (0x0000, 1, 0),
        (0x0001, len(strings), off_strings),
        (0x0002, len(types), off_types),
        (0x0003, len(protos), off_protos),
        (0x0005, len(methods), off_methods),
        (0x0006, 1, off_classdefs),
    ] + map_entries + [(0x1000, 1, map_off)]
    ordered.sort(key=lambda e: e[2])
    data += struct.pack('<I', len(ordered))
    for typ, count, off in ordered:
        data += struct.pack('<HHII', typ, 0, count, off)

    # --- assemble ------------------------------------------------------------
    file_size = off_data + len(data)
    out = bytearray(file_size)
    out[0:8] = b'dex\n035\x00'
    struct.pack_into('<' + 'I' * 20, out, 32,
                     file_size, DEX_HEADER_SIZE, 0x12345678, 0, 0, map_off,
                     len(strings), off_strings,
                     len(types), off_types,
                     len(protos), off_protos,
                     0, 0,                                    # field ids
                     len(methods), off_methods,
                     1, off_classdefs,
                     len(data), off_data)
    for i, _ in enumerate(strings):
        struct.pack_into('<I', out, off_strings + 4 * i, string_data_off[i])
    for i, t in enumerate(types):
        struct.pack_into('<I', out, off_types + 4 * i, sidx[t])
    for i, (key, (ret, params)) in enumerate(protos):
        struct.pack_into('<III', out, off_protos + 12 * i,
                         sidx[shorty(ret, params)], tidx[ret], type_list_off.get(key, 0))
    for i, (cls, name, key) in enumerate(methods):
        struct.pack_into('<HHI', out, off_methods + 8 * i, tidx[cls], pidx[key], sidx[name])
    struct.pack_into('<IIIIIIII', out, off_classdefs,
                     tidx[main_desc], 0x0001, tidx[ACT], 0, NO_INDEX, 0, class_data_off, 0)
    out[off_data:] = data

    out[12:32] = hashlib.sha1(bytes(out[32:])).digest()
    struct.pack_into('<I', out, 8, adler32(bytes(out[12:])))
    return bytes(out)


# ---------------------------------------------------------------------------
# APK Signature Scheme v2 (inserted as an APK Signing Block before the
# central directory). Modern Android installers expect this block; v1 alone
# is not accepted by every Android version.
# ---------------------------------------------------------------------------
V2_BLOCK_ID = 0x7109871A
SIG_ALG_RSA_PKCS1_SHA256 = 0x0103
APK_SIG_MAGIC = b'APK Sig Block 42'
CHUNK = 1024 * 1024


def lp(b):
    return struct.pack('<I', len(b)) + b


def _openssl_bytes(args, stdin_bytes=None):
    return subprocess.run(args, input=stdin_bytes, stdout=subprocess.PIPE, check=True).stdout


def add_v2_signature(apk, key, cert):
    """Return apk bytes with a v2 signing block inserted."""
    eocd = len(apk) - 22
    if apk[eocd:eocd + 4] != b'PK\x05\x06':
        raise ValueError('EOCD not found at end of APK (zip comment not supported)')
    cd_off = struct.unpack_from('<I', apk, eocd + 16)[0]
    sec_a = apk[:cd_off]
    sec_b = apk[cd_off:eocd]

    cert_der = _openssl_bytes(['openssl', 'x509', '-in', cert, '-outform', 'DER'])
    pub_der = _openssl_bytes(['openssl', 'x509', '-in', cert, '-pubkey', '-noout'])
    pub_der = _openssl_bytes(['openssl', 'pkey', '-pubin', '-outform', 'DER'], pub_der)

    # Digest of the file contents: 3 sections, each split into 1 MiB chunks.
    # The EOCD is hashed with its central-directory offset pointing at the signing block.
    block_pairs_len_placeholder = None

    def chunk_digests(parts):
        digests = []
        for part in parts:
            for i in range(0, len(part), CHUNK):
                c = part[i:i + CHUNK]
                digests.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
        return digests

    # First pass to know the signing-block size (it does not depend on the digest value).
    dummy_digest = b'\x00' * 32
    def build_signer(content_digest, sign_fn):
        digests_seq = lp(struct.pack('<I', SIG_ALG_RSA_PKCS1_SHA256) + lp(content_digest))
        certs_seq = lp(cert_der)
        signed_data = lp(digests_seq) + lp(certs_seq) + lp(b'')
        signature = sign_fn(signed_data)
        sigs_seq = lp(struct.pack('<I', SIG_ALG_RSA_PKCS1_SHA256) + lp(signature))
        return lp(signed_data) + lp(sigs_seq) + lp(pub_der), signed_data

    def make_block(content_digest, sign_fn):
        signer, _ = build_signer(content_digest, sign_fn)
        value = lp(lp(signer))  # sequence of length-prefixed signers
        pair = struct.pack('<Q', 4 + len(value)) + struct.pack('<I', V2_BLOCK_ID) + value
        size = len(pair) + 24
        return struct.pack('<Q', size) + pair + struct.pack('<Q', size) + APK_SIG_MAGIC

    def sign_with_key(data):
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, 'signed')
            with open(p, 'wb') as f:
                f.write(data)
            return _openssl_bytes(['openssl', 'dgst', '-sha256', '-sign', key, p])

    # The EOCD bytes change with the new CD offset, so compute with the final offset.
    block_len = len(make_block(dummy_digest, lambda d: b'\x00' * 256))
    new_eocd = bytearray(apk[eocd:])
    struct.pack_into('<I', new_eocd, 16, cd_off + block_len)
    digests = chunk_digests([sec_a, sec_b, bytes(new_eocd)])
    top = hashlib.sha256(b'\x5a' + struct.pack('<I', len(digests)) + b''.join(digests)).digest()
    block = make_block(top, sign_with_key)
    assert len(block) == block_len, 'signing block size changed'
    return sec_a + block + sec_b + bytes(new_eocd)


# ---------------------------------------------------------------------------
# Signing (JAR / v1 scheme with openssl)
# ---------------------------------------------------------------------------
def ensure_keys():
    os.makedirs(SIGNING_DIR, exist_ok=True)
    key = os.path.join(SIGNING_DIR, 'debug-key.pem')
    cert = os.path.join(SIGNING_DIR, 'debug-cert.pem')
    if not (os.path.exists(key) and os.path.exists(cert)):
        subprocess.check_call([
            'openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', key, '-out', cert, '-days', '10000',
            '-subj', '/CN=Code Reviewer/O=SPORstudio/C=MY',
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return key, cert


def sign_entries(entries, key, cert):
    """entries: list of (name, bytes). Returns list of extra META-INF entries."""
    manifest = 'Manifest-Version: 1.0\r\nCreated-By: 1.0 (Code Reviewer build)\r\n\r\n'
    sections = {}
    for name, content in entries:
        section = 'Name: %s\r\nSHA-256-Digest: %s\r\n\r\n' % (
            name, base64.b64encode(hashlib.sha256(content).digest()).decode())
        sections[name] = section
        manifest += section
    manifest_bytes = manifest.encode('utf-8')

    sf = ('Signature-Version: 1.0\r\nCreated-By: 1.0 (Code Reviewer build)\r\n'
          'SHA-256-Digest-Manifest: %s\r\n\r\n' % base64.b64encode(
              hashlib.sha256(manifest_bytes).digest()).decode())
    for name, _ in entries:
        sf += 'Name: %s\r\nSHA-256-Digest: %s\r\n\r\n' % (
            name, base64.b64encode(hashlib.sha256(sections[name].encode('utf-8')).digest()).decode())
    sf_bytes = sf.encode('utf-8')

    with tempfile.TemporaryDirectory() as td:
        sf_path = os.path.join(td, 'CERT.SF')
        rsa_path = os.path.join(td, 'CERT.RSA')
        with open(sf_path, 'wb') as f:
            f.write(sf_bytes)
        subprocess.check_call([
            'openssl', 'smime', '-sign', '-binary', '-md', 'sha256',
            '-in', sf_path, '-signer', cert, '-inkey', key,
            '-outform', 'DER', '-out', rsa_path,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(rsa_path, 'rb') as f:
            rsa_bytes = f.read()
    return [('META-INF/MANIFEST.MF', manifest_bytes),
            ('META-INF/CERT.SF', sf_bytes),
            ('META-INF/CERT.RSA', rsa_bytes)]


DOWNLOAD_PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Download Code Reviewer</title>
<style>
  body { margin: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center;
    font-family: -apple-system, "Segoe UI", Roboto, Arial, sans-serif; background: #f4f6fb; color: #1d2433; }
  main { background: #fff; border-radius: 14px; padding: 28px; max-width: 460px; width: calc(100% - 40px);
    box-shadow: 0 6px 24px #0001; text-align: center; }
  h1 { margin: 0 0 6px; font-size: 22px; }
  p { color: #6b7388; line-height: 1.45; }
  a.btn { display: inline-block; margin-top: 14px; background: #3b5bdb; color: #fff; text-decoration: none;
    padding: 12px 22px; border-radius: 10px; font-weight: 600; }
  code { font-size: 11px; word-break: break-all; color: #6b7388; }
  small { display: block; margin-top: 16px; color: #6b7388; }
</style>
</head>
<body>
<main>
  <h1>Code Reviewer</h1>
  <p>Rule-based code review for Android. Paste code, get errors and recommendations. No AI, works offline.</p>
  <a class="btn" id="dl" href="CodeReviewer.apk" download="CodeReviewer.apk">Download APK (__SIZE__ KB)</a>
  <small>Download starts automatically. If it does not, tap the button above.</small>
  <small>Version __VERSION__ · Android __MIN__+ · SHA-256 <code>__SHA__</code></small>
  <small>To install: open the file and allow "Install unknown apps" for your browser.</small>
</main>
<script>
  // Start the download as soon as the page opens.
  window.addEventListener('load', function () {
    setTimeout(function () { document.getElementById('dl').click(); }, 300);
  });
</script>
</body>
</html>
"""


def write_download_page(apk_path):
    sha = hashlib.sha256(open(apk_path, 'rb').read()).hexdigest()
    html = (DOWNLOAD_PAGE.replace('__SIZE__', str(max(1, os.path.getsize(apk_path) // 1024)))
            .replace('__VERSION__', VERSION_NAME).replace('__MIN__', str(MIN_SDK)).replace('__SHA__', sha))
    with open(os.path.join(RELEASE_DIR, 'index.html'), 'w', encoding='utf-8') as f:
        f.write(html)
    return sha


def main():
    if not os.path.isdir(ASSETS_DIR):
        sys.exit('Missing assets directory: ' + ASSETS_DIR)
    os.makedirs(RELEASE_DIR, exist_ok=True)

    prebuilt_dex = os.path.join(HERE, 'classes.dex')
    if os.path.exists(prebuilt_dex):
        # Preferred: DEX assembled by smali from ../smali/ (see README).
        with open(prebuilt_dex, 'rb') as f:
            dex = f.read()
    else:
        dex = build_dex()
    entries = [
        ('AndroidManifest.xml', build_manifest()),
        ('classes.dex', dex),
    ]
    for root, _, files in os.walk(ASSETS_DIR):
        for fn in sorted(files):
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, ASSETS_DIR).replace(os.sep, '/')
            with open(full, 'rb') as f:
                entries.append(('assets/' + rel, f.read()))

    key, cert = ensure_keys()
    entries += sign_entries(entries, key, cert)

    out_path = os.path.join(RELEASE_DIR, APK_NAME)
    if os.path.exists(out_path):
        os.remove(out_path)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, content in entries:
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 6, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, content)
    with open(out_path, 'wb') as f:
        f.write(add_v2_signature(buf.getvalue(), key, cert))
    sha = write_download_page(out_path)
    print('Wrote', out_path, os.path.getsize(out_path), 'bytes, sha256', sha)


if __name__ == '__main__':
    main()

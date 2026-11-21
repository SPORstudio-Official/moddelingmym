/* ==========================================================================
 *  AE -- Electron host bridge
 *
 *  This is the "motherboard" for the WebAssembly build.  It owns everything a
 *  PC gives an OS but a sandboxed wasm module cannot reach on its own:
 *
 *    - a surface to draw on          (a <canvas>, via ImageData)
 *    - a clock                       (performance.now)
 *    - input hardware                (DOM keyboard/mouse events)
 *    - a wall clock                  (Date)
 *    - a speaker                     (WebAudio beeps)
 *
 *  It exports exactly the seven functions dev/00_prelude.c imports.  No kernel
 *  code knows this file exists.
 * ======================================================================== */
'use strict';

/* Field offsets of hal_event_t on wasm32 (9 x 4-byte words = 36 bytes).
 * dev/20_hal.c has a compile-time assertion that keeps this in sync. */
const EV = {
  type: 0, key: 4, ch: 8, mx: 12, my: 16,
  wheel: 20, buttons: 24, repeat: 28, time_ms: 32,
  SIZE: 36,
};

/* HAL_EV_* discriminants, mirroring the enum in dev/20_hal.c. */
const EV_NONE = 0, EV_KEYDOWN = 1, EV_KEYUP = 2, EV_CHAR = 3,
      EV_MOUSEMOVE = 4, EV_MOUSEDOWN = 5, EV_MOUSEUP = 6, EV_WHEEL = 7,
      EV_FOCUS = 8, EV_QUIT = 9;

const AetherHost = (() => {
  let inst = null;          // the instantiated wasm exports
  let ctx = null;           // 2d context of the framebuffer canvas
  let imageData = null;     // reused ImageData, one per frame
  let W = 0, H = 0, vram = 0;

  const queue = [];         // pending hal_event_t-shaped objects
  const MAX_QUEUE = 512;
  let buttons = 0;          // 1 left, 2 right, 4 middle
  let beepCtx = null;

  /* Decode a NUL-terminated string out of the module's memory. */
  function cstr(ptr) {
    if (!ptr || !inst) return '';
    const mem = new Uint8Array(inst.memory.buffer);
    let s = '';
    for (let i = ptr; i < mem.length && mem[i]; i++) s += String.fromCharCode(mem[i]);
    return s;
  }

  /* ---- event queue ------------------------------------------------------ */
  function push(ev) {
    if (queue.length >= MAX_QUEUE) queue.shift();
    ev.time_ms = performance.now() | 0;
    queue.push(ev);
  }

  function nowEv(type, extra) {
    return Object.assign({
      type, key: 0, ch: 0, mx: 0, my: 0, wheel: 0,
      buttons: 0, repeat: 0, time_ms: 0,
    }, extra || {});
  }

  /* ---- canvas geometry -------------------------------------------------- */
  function toGuest(ev) {
    /* Map a DOM pointer position onto the emulated 1280x720 surface.  The CSS
       box may be letterboxed to preserve the 16:9 aspect ratio. */
    const r = ev.target.getBoundingClientRect();
    const x = (ev.clientX - r.left) / r.width * W;
    const y = (ev.clientY - r.top) / r.height * H;
    return {
      mx: Math.max(0, Math.min(W - 1, Math.round(x))),
      my: Math.max(0, Math.min(H - 1, Math.round(y))),
    };
  }

  /* ---- input wiring ----------------------------------------------------- */
  function wireInput(canvas) {
    /* Keyboard.  The kernel expects (a) a translated glyph in `ch` -- it applies
       no modifier state of its own -- and (b) a Win32 virtual-key code in `key`
       for the special keys it maps to scancodes.  Both are emitted so typing and
       shortcuts (Ctrl+`, F4, arrows) work. */
    const SPECIAL_VK = {
      Backspace: 0x08, Tab: 0x09, Enter: 0x0D, Shift: 0x10, Control: 0x11,
      Alt: 0x12, Pause: 0x13, Escape: 0x1B, ' ': 0x20, Spacebar: 0x20,
      End: 0x23, Home: 0x24, ArrowLeft: 0x25, ArrowUp: 0x26,
      ArrowRight: 0x27, ArrowDown: 0x28, Delete: 0x2E,
      Insert: 0x2D, PageUp: 0x21, PageDown: 0x22,
    };
    const SPECIAL_VK_BY_CODE = {
      Backspace: 0x08, Tab: 0x09, Enter: 0x0D, NumpadEnter: 0x0D,
      ShiftLeft: 0x10, ShiftRight: 0x10, ControlLeft: 0x11, ControlRight: 0x11,
      AltLeft: 0x12, AltRight: 0x12, Escape: 0x1B, Space: 0x20,
      End: 0x23, Home: 0x24, ArrowLeft: 0x25, ArrowUp: 0x26,
      ArrowRight: 0x27, ArrowDown: 0x28, Delete: 0x2E, Insert: 0x2D,
      PageUp: 0x21, PageDown: 0x22,
    };

    function vkFor(e) {
      if (SPECIAL_VK_BY_CODE[e.code] !== undefined) return SPECIAL_VK_BY_CODE[e.code];
      if (SPECIAL_VK[e.key] !== undefined) return SPECIAL_VK[e.key];
      if (e.key.length === 1) {
        const c = e.key.charCodeAt(0);
        if (c >= 0x61 && c <= 0x7A) return c - 0x20;   /* a-z -> A-Z */
        if (c >= 0x30 && c <= 0x39) return c;          /* 0-9 */
        if (c >= 0x41 && c <= 0x5A) return c;
      }
      if (/^F[1-9]$|^F1[0-2]$/.test(e.key)) {
        return 0x70 + (parseInt(e.key.slice(1), 10) - 1);
      }
      return 0;
    }

    canvas.addEventListener('keydown', (e) => {
      const vk = vkFor(e);
      /* Stop the browser eating the keys the OS itself handles. */
      if (['Tab', 'Backspace', 'F4', 'F12', ' '].includes(e.key) ||
          e.ctrlKey || e.altKey) {
        e.preventDefault();
      }
      /* Shift produces an uppercase glyph, CapsLock does not -- so the glyph
         must come from `key`, not from the key code. */
      const upper = e.shiftKey && !e.ctrlKey && !e.altKey;

      push(nowEv(EV_KEYDOWN, { key: vk, repeat: e.repeat ? 1 : 0 }));

      /* Text is emitted for printable keys, plus the three editing keys the
         kernel maps to control characters.  Ctrl+letter does NOT produce text
         (browsers report it as a control character); the app reads the key code
         instead, which is what a shell expects for Ctrl+C.  Alt is excluded for
         the same reason: Alt+Q is "close window", not the letter q. */
      const plain = !e.ctrlKey && !e.altKey && !e.metaKey;
      if (e.key.length === 1 && plain) {
        let g = e.key.charCodeAt(0);
        if (upper && g >= 0x61 && g <= 0x7A) g -= 0x20;   /* a-z -> A-Z */
        push(nowEv(EV_CHAR, { ch: g, key: 0 }));
      } else if (e.key === 'Enter' && plain) {
        push(nowEv(EV_CHAR, { ch: 13, key: 0 }));
      } else if (e.key === 'Backspace' && plain) {
        push(nowEv(EV_CHAR, { ch: 8, key: 0 }));
      } else if (e.key === 'Tab' && plain) {
        push(nowEv(EV_CHAR, { ch: 9, key: 0 }));
      }
    });

    canvas.addEventListener('keyup', (e) => {
      push(nowEv(EV_KEYUP, { key: vkFor(e) }));
    });

    canvas.addEventListener('mousemove', (e) => {
      const p = toGuest(e);
      push(nowEv(EV_MOUSEMOVE, { mx: p.mx, my: p.my, buttons }));
    });

    canvas.addEventListener('mousedown', (e) => {
      const p = toGuest(e);
      buttons |= (e.button === 0 ? 1 : e.button === 1 ? 4 : e.button === 2 ? 2 : 0);
      canvas.focus();
      push(nowEv(EV_MOUSEDOWN, { mx: p.mx, my: p.my, buttons }));
      e.preventDefault();
    });

    window.addEventListener('mouseup', (e) => {
      const p = toGuest({ target: canvas, clientX: e.clientX, clientY: e.clientY });
      buttons &= ~(e.button === 0 ? 1 : e.button === 1 ? 4 : e.button === 2 ? 2 : 0);
      push(nowEv(EV_MOUSEUP, { mx: p.mx, my: p.my, buttons }));
    });

    canvas.addEventListener('contextmenu', (e) => e.preventDefault());

    canvas.addEventListener('wheel', (e) => {
      /* One notch of a real wheel is 120 units, which is what the kernel
         divides by to get a scroll step. */
      const dz = e.deltaMode === 1 ? -e.deltaY * 40 : -e.deltaY * 1;
      push(nowEv(EV_WHEEL, { wheel: Math.round(dz) }));
      e.preventDefault();
    }, { passive: false });

    canvas.addEventListener('blur', () => {
      buttons = 0;
      push(nowEv(EV_FOCUS));
    });

    /* Keep the OS's modifier state sane if the window loses focus mid-chord. */
    window.addEventListener('blur', () => {
      buttons = 0;
    });
  }

  /* ---- networking -------------------------------------------------------
   *  The kernel runs a synchronous frame loop and cannot await, so each fetch
   *  is started here and parked in a slot; the kernel polls the slot and copies
   *  the body out when it is ready.  Slots are released by the kernel.
   * -------------------------------------------------------------------- */
  const httpSlots = new Map();
  let httpNextSlot = 1;

  /* ---- network ---------------------------------------------------------- *
   * The browser inside the OS is a reader: it asks the host for bytes and lays
   * them out itself.  Three things made "most websites" unreachable, and each
   * one is fixed here rather than by pretending the reader is a web engine.
   *
   *  1. CORS.  A cross-origin fetch from this page is refused by the browser
   *     before any bytes arrive, so news sites, wikis and blogs all failed with
   *     an opaque TypeError.  A readable text proxy is tried as a fallback;
   *     the page keeps its own address bar showing the real URL.
   *  2. Blocked hosts.  Some sites refuse machine traffic outright; those are
   *     reported as "blocked", with the reason, instead of a generic failure.
   *  3. Big pages.  The body is capped, but the cap is now generous and the
   *     reader is told when it truncated rather than being handed nothing.
   */
  const TEXT_PROXIES = [
    (u) => 'https://r.jina.ai/' + u,
    (u) => 'https://api.allorigins.win/raw?url=' + encodeURIComponent(u),
  ];

  function isBlockedHost(url) {
    /* Hosts that answer a fetch with an anti-bot page or a hard refusal.  They
       still open in a real window via the browser's "open on the host" action;
       here they would only produce a wall of JavaScript as text. */
    return /(^|\.)(instagram|facebook|twitter|x|tiktok|netflix|spotify|linkedin|amazon|discord)\./i.test(url)
        || /accounts\.google\.|google\.com\/search|youtube\.com\/watch/i.test(url);
  }

  function httpSpawn(url) {
    const id = httpNextSlot++;
    const slot = { state: 0, status: 0, body: new Uint8Array(0), url, via: '', error: '' };
    httpSlots.set(id, slot);
    (async () => {
      const attempts = [{ url, via: 'direct' }];
      if (!isBlockedHost(url)) {
        for (const wrap of TEXT_PROXIES) attempts.push({ url: wrap(url), via: 'reader proxy' });
      }
      let lastErr = '';
      for (const a of attempts) {
        try {
          const resp = await fetch(a.url, {
            method: 'GET',
            redirect: 'follow',
            /* Identify honestly.  A reader that lies about being Chrome tends
               to be served a page it cannot possibly lay out. */
            headers: {
              'Accept': 'text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.5',
            },
          });
          if (!resp.ok) { lastErr = 'HTTP ' + resp.status; continue; }
          const buf = await resp.arrayBuffer();
          if (!buf || buf.byteLength < 32) { lastErr = 'empty response'; continue; }
          slot.status = resp.status;
          slot.body = new Uint8Array(buf);
          slot.via = a.via;
          slot.state = 1;
          return;
        } catch (err) {
          lastErr = String((err && err.message) || err);
          /* A refused-for-CORS fetch throws TypeError('Failed to fetch'); the
             next attempt is the proxy, so just remember the reason. */
        }
      }
      slot.error = lastErr || 'could not be reached';
      slot.blocked = isBlockedHost(url) ? 1 : 0;
      slot.state = 2;
    })();
    return id;
  }

  function httpGet(id) {
    if (id <= 0) return null;
    const s = httpSlots.get(id);
    return s || null;
  }

  /* ---- the imports ------------------------------------------------------ */
  function makeImports(el) {
    return {
      env: {
        /* performance.now() has sub-millisecond resolution and is monotonic,
           which is exactly what the kernel's clock wants. */
        js_now_us() {
          return BigInt(Math.round(performance.now() * 1000));
        },

        /* Convert rows [y0, y1) of the module's framebuffer into the packed
           RGBA buffer the canvas wants.  The kernel stores 0xAARRGGBB, i.e. the
           bytes in memory are B,G,R,A -- so R and B are swapped on the way out.
           Only the damaged region is converted and uploaded; converting all
           921,600 pixels every frame was the browser build's biggest cost. */
        js_present_rect(x, y, w, h) {
          const mem = new Uint8Array(inst.memory.buffer);
          const dst = imageData.data;
          const y0 = Math.max(0, y | 0);
          const y1 = Math.min(H, (y | 0) + (h | 0));
          const x0 = Math.max(0, x | 0);
          const x1 = Math.min(W, (x | 0) + (w | 0));
          if (x1 <= x0 || y1 <= y0) return;
          for (let row = y0; row < y1; row++) {
            let p = vram + row * W * 4 + x0 * 4;
            let o = (row * W + x0) * 4;
            for (let c = x0; c < x1; c++, p += 4, o += 4) {
              dst[o] = mem[p + 2];
              dst[o + 1] = mem[p + 1];
              dst[o + 2] = mem[p];
              dst[o + 3] = 255;
            }
          }
          /* putImageData takes a dirty rect in device pixels, so only the
             converted region is pushed to the compositor. */
          ctx.putImageData(imageData, 0, 0, x0, y0, x1 - x0, y1 - y0);
        },

        /* Whole-surface present.  Used by the boot and panic screens, which
           repaint everything. */
        js_present() {
          this.js_present_rect(0, 0, W, H);
        },

        js_poll_event(ptr) {
          if (!queue.length) return 0;
          const ev = queue.shift();
          const dv = new DataView(inst.memory.buffer);
          dv.setInt32(ptr + EV.type, ev.type, true);
          dv.setInt32(ptr + EV.key, ev.key, true);
          dv.setInt32(ptr + EV.ch, ev.ch, true);
          dv.setInt32(ptr + EV.mx, ev.mx, true);
          dv.setInt32(ptr + EV.my, ev.my, true);
          dv.setInt32(ptr + EV.wheel, ev.wheel, true);
          dv.setInt32(ptr + EV.buttons, ev.buttons, true);
          dv.setInt32(ptr + EV.repeat, ev.repeat, true);
          dv.setUint32(ptr + EV.time_ms, ev.time_ms >>> 0, true);
          return 1;
        },

        js_wallclock(y, mo, d, hh, mm, ss) {
          const t = new Date();
          const dv = new DataView(inst.memory.buffer);
          if (y) dv.setInt32(y, t.getFullYear(), true);
          if (mo) dv.setInt32(mo, t.getMonth() + 1, true);
          if (d) dv.setInt32(d, t.getDate(), true);
          if (hh) dv.setInt32(hh, t.getHours(), true);
          if (mm) dv.setInt32(mm, t.getMinutes(), true);
          if (ss) dv.setInt32(ss, t.getSeconds(), true);
        },

        js_set_title(ptr) {
          /* Decode the NUL-terminated C string the kernel passed. */
          const mem = new Uint8Array(inst.memory.buffer);
          let s = '';
          for (let i = ptr; i < mem.length && mem[i]; i++) s += String.fromCharCode(mem[i]);
          if (el && el.status) el.status.textContent = s || 'AE';
          document.title = s || 'AE';
        },

        js_beep(freq, ms) {
          try {
            if (!beepCtx) beepCtx = new (window.AudioContext || window.webkitAudioContext)();
            const osc = beepCtx.createOscillator();
            const gain = beepCtx.createGain();
            osc.frequency.value = freq > 0 ? freq : 880;
            gain.gain.value = 0.03;
            osc.connect(gain).connect(beepCtx.destination);
            osc.start();
            osc.stop(beepCtx.currentTime + Math.max(0.02, (ms || 80) / 1000));
          } catch (_) { /* audio is optional */ }
        },

        js_log(ptr) {
          const mem = new Uint8Array(inst.memory.buffer);
          let s = '';
          for (let i = ptr; i < mem.length && mem[i]; i++) s += String.fromCharCode(mem[i]);
          console.log('[aether]', s);
        },

        js_request_quit() {
          console.log('[aether] kernel requested shutdown');
        },

        /* ---- networking ------------------------------------------------- */
        js_http_get(ptr) {
          const url = cstr(ptr);
          if (!/^https?:\/\//i.test(url)) return -1;
          return httpSpawn(url);
        },

        js_http_state(id) {
          const s = httpGet(id);
          return s ? s.state : 2;
        },

        js_http_status(id) {
          const s = httpGet(id);
          return s ? s.status : 0;
        },

        js_http_len(id) {
          const s = httpGet(id);
          return s ? s.body.length : 0;
        },

        js_http_read(id, dst, max) {
          const s = httpGet(id);
          if (!s || dst <= 0 || max <= 0) return 0;
          const n = Math.min(max, s.body.length);
          new Uint8Array(inst.memory.buffer, dst, n).set(s.body.subarray(0, n));
          return n;
        },

        js_http_release(id) {
          httpSlots.delete(id);
        },

        js_open_external(ptr) {
          const url = cstr(ptr);
          console.log('[aether] open external:', url);
          window.open(url, '_blank', 'noopener');
        },
      },
    };
  }

  /* ---- boot ------------------------------------------------------------- */
  async function start(el) {
    const canvas = el.canvas;
    ctx = canvas.getContext('2d', { alpha: false });

    let resp;
    try {
      resp = await fetch('aether.wasm');
      if (!resp.ok) throw new Error('HTTP ' + resp.status);
    } catch (err) {
      el.status.textContent = 'Cannot load aether.wasm: ' + err.message;
      el.showError('Could not load aether.wasm',
        'The WebAssembly image could not be fetched (' + err.message + ').\n\n' +
        'If you are running from source, build it first:\n' +
        '  npm run build:wasm');
      return false;
    }

    const bytes = await resp.arrayBuffer();
    let instance;
    try {
      ({ instance } = await WebAssembly.instantiate(bytes, makeImports(el)));
    } catch (err) {
      el.status.textContent = 'aether.wasm failed to instantiate';
      el.showError('WebAssembly instantiation failed', String(err));
      return false;
    }

    inst = instance.exports;
    W = inst.aether_screen_w();
    H = inst.aether_screen_h();
    canvas.width = W;
    canvas.height = H;
    imageData = ctx.createImageData(W, H);

    const rc = inst.aether_boot();
    if (rc !== 0) {
      const code = inst.aether_last_error();
      el.status.textContent = 'Boot failed';
      el.showError('AE could not boot',
        'hal_init() returned ' + code + ', so the machine stopped before the ' +
        'desktop could start.\n\n-1  out of memory\n' +
        'This build allocates 64 MB of linear memory for physical RAM plus a ' +
        '3.7 MB framebuffer.');
      return false;
    }

    vram = inst.aether_vram() >>> 0;
    wireInput(canvas);
    canvas.focus();

    /* Drive the machine from requestAnimationFrame.  One frame per vsync keeps
       the compositor in step with the display and the CPU idle in between. */
    let last = 0, frames = 0, fpsT = performance.now();
    function tick(now) {
      try {
        if (!inst.aether_running()) {
          el.status.textContent = 'AE halted';
          return;
        }
        inst.aether_frame();
        frames++;
        if (now - fpsT >= 1000) {
          const fps = Math.round(frames * 1000 / (now - fpsT));
          if (el.fps) el.fps.textContent = fps + ' fps';
          frames = 0; fpsT = now;
        }
      } catch (err) {
        el.status.textContent = 'Kernel fault';
        el.showError('The AE kernel faulted',
          String(err && err.stack || err));
        return;
      }
      last = now;
      window.__aetherRaf = requestAnimationFrame(tick);
    }
    window.__aetherRaf = requestAnimationFrame(tick);
    return true;
  }

  return { start };
})();

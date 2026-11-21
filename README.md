# AE — Architecture Environment

A complete 32-bit operating system — kernel, virtual file system, process
scheduler, bytecode VM, window manager, and GUI applications — that boots inside
a single Windows executable with its own icon and its own identity.

The whole OS is a freestanding C11 program. It links **no C runtime**: string,
memory, formatting, heap and floating-point routines are all implemented in the
kernel itself, so the built executable imports nothing but four Win32 system
libraries (`kernel32`, `user32`, `gdi32`, `winmm`).

`dist/AE.exe` is the whole machine: a 10 KB **launcher** carrying the OS, plus a
386 KB **resource section** holding the AE icon (16-256 px) and its version
metadata. The launcher opens the OS in an app-mode window with its own
AppUserModelID, its own profile in `%LOCALAPPDATA%\AE`, and no browser
first-run, sync or "restore pages" prompts -- so Windows shows **AE**, in the
taskbar, the Start menu and in search, not the engine underneath it.

## Build

```sh
tools/setup_toolchain.sh     # fetches zig 0.17.1 into ~/.local/zig (once)
tools/build_exe.sh           # -> dist/AE.exe (one file, own icon + metadata)
tools/build.sh               # -> the 337 KB kernel-only Windows build
tools/build.sh --headless    # -> Linux binary for CI/screenshot runs
```

Requires nothing else: `zig cc` cross-compiles the Windows PE from Linux with no
MSVC or mingw installation.

## Layout

The kernel is one translation unit assembled from ordered modules in `dev/`.
`tools/build.sh` concatenates them into `aether_os.c` and compiles that.

| Module | Contents |
| --- | --- |
| `dev/00_prelude.c` | portable types, target detection, Win32 includes, macros |
| `dev/font_data.h` | generated glyph bitmaps (see `tools/genfont.py`) |
| `dev/10_util.c` | libc subset, floating-point maths, kernel log |
| `dev/20_hal.c` | Win32 hardware layer, or the headless script-driven chassis |
| `dev/30_irq.c` | 8259 PIC remap, 1000 Hz timer, IRQ accounting |
| `dev/40_gfx.c` | framebuffer, clipping, shapes, text, cursors |
| `dev/50_mm.c` | physical heap allocator |
| `dev/60_vfs.c` | inodes, blocks, directories, seeded filesystem |
| `dev/65_proc.c` | process control blocks, scheduler, accounting |
| `dev/67_procfs.c` | `/proc`-style introspection |
| `dev/70_bytecode.c` | the `cc` C-subset compiler and stack-machine VM |
| `dev/75_registry.c` | settings persistence |
| `dev/80_wm.c` | window manager, compositor, taskbar, Start menu, toasts |
| `dev/85_app_sh.c` | Crommand (command interpreter, `cc`, `run`) |
| `dev/86..93_app_*.c` | sysmon, explorer, editor, hexview, paint, settings, calc, console |
| `dev/95_main.c` | boot sequence, panic handler, main loop |

## What it does

- **Crommand** -- command interpreter with `ls`, `cat`, `cp`, `mv`, `rm`,
  `stat`, `hexdump`, `df`, `free`, `ps`, `top`, `dmesg`, `uname`, `theme`, plus
  `cc`/`run` for the built-in compiler.
- **`cc` + bytecode VM** -- compiles a C subset (functions, recursion, loops,
  `print`, framebuffer calls) and runs it as a real PID. `fib(0..19)` is
  computed recursively inside the VM.
- **Window manager** -- drag, resize, snap, minimize, maximize, z-order, taskbar
  with live CPU/RAM tray, Start menu, toasts, four themes and four wallpapers.
- **Apps** -- System Monitor (live graphs, heap block map, VFS map, process kill),
  File Explorer, Code Studio editor with syntax highlighting, Hex Inspector,
  Paint Studio, Control Panel, Calculator, Program Console.

## Running it on your desktop (Electron shell)

The same kernel also runs inside a desktop window. `tools/build_wasm.sh`
compiles the identical modules to a freestanding WebAssembly module, and the
Electron shell in `app/` + `electron/` is its chassis: it provides the surface,
the clock, input and the speaker -- the four things a real motherboard provides
and a sandboxed wasm module cannot reach.

```sh
npm install
npm start          # builds aether.wasm, then opens the AE window
```

`npm start` rebuilds the module every time, so the window can never run a stale
kernel. To re-open without rebuilding use `npm run start:only`.

| Command | What it does |
| --- | --- |
| `npm run build:wasm` | Modules -> `app/aether.wasm` (freestanding wasm32) |
| `npm start` | Build, then launch the Electron window |
| `npm run web` | Serve the shell on <http://localhost:8080> for a plain browser |
| `npm test` | Headless host-bridge test: boots the kernel and drives it with synthetic input |
| `npm run dist` | Package a standalone installer with electron-builder |

### How the port works

No OS code knows the shell exists. Only the HAL gained a third back end, next to
the Win32 and headless ones:

- **memory** -- physical RAM is a grown region of the module's linear memory,
  so the module's address space *is* the machine's (no `VirtualAlloc`).
- **display** -- the compositor writes ARGB pixels into a framebuffer in that
  same memory; the shell copies it into a canvas `ImageData` per frame.
- **time** -- `js_now_us()` wraps `performance.now()`, and the kernel's own IRQ0
  derives its 1 kHz ticks from it exactly as it does on Windows.
- **input** -- DOM events are marshalled into `hal_event_t` (36 bytes, layout
  pinned by a compile-time assertion) and decoded by the kernel's existing
  scancode translation.
- **frame stepping** -- a wasm module must not block its host, so the frame loop
  was split into `aether_boot()` and `aether_frame()`. Electron calls the latter
  from `requestAnimationFrame`; the Win32 and headless builds keep the original
  blocking loop in `aether_main()`.

### A note on the keyboard bug this fixed

Sending both a key transition *and* the translated glyph made every character
appear twice ("abc" -> "aabbcc"), because the kernel also derives a glyph from
the scancode. The transition now carries no glyph, and the host-resolved glyph
is authoritative -- matching Win32, where `WM_CHAR` follows `WM_KEYDOWN`. The
Win32 and headless chassis are unaffected.

### A standalone app, no install

```sh
npm run exe        # -> dist/AE.exe (Windows, single file, double-click)
npm run app        # -> dist/AE.html + launchers for every platform
```

`dist/AE.exe` is a real Windows executable, not a shortcut: a 10 KB native
launcher with the whole 1.4 MB machine appended to it as a payload. Double-click
it and AE opens in its own window. It needs no installer, no runtime, no
Node, no Electron and no network. The launcher imports only `KERNEL32`,
`ADVAPI32`, `SHELL32` and `USER32` -- it has no C runtime dependency, so it runs
on any 64-bit Windows from 7 onward. On start it finds the `<!DOCTYPE html>`
anchor, writes the payload to `%TEMP%\AE.html`, and opens that file with
its own private browser profile via a Chromium-family browser in `--app` mode;
if no such browser exists it hands the file to the default browser instead, and
every failure path shows a normal Windows message box rather than failing
silently.

`dist/AE.html` is the whole machine in one 1.4 MB file: kernel,
compositor, VFS, scheduler, C compiler, every app and the browser. Double-click
it and it runs, offline. The generated launchers open it in a chrome-less app
window so it feels like a program rather than a web page:

| Platform | Launcher |
| --- | --- |
| Windows | `dist\AE.exe` (double-click), or `dist\AE.cmd` for the script version |
| macOS | `dist/AE.command` |
| Linux | `dist/aether-os.sh`, or install `AE.desktop` |

Electron (`npm start`) is still there for a true native window, but it needs a
200 MB platform-specific download and, to distribute, code signing. For an OS
that already runs in any browser engine, one HTML file is the honest standalone
build.

### Customising it

Everything is changeable from the Control Panel's Appearance tab or from
Crommand, and every change is written to `/etc/desktop.cfg` so it survives a
reboot:

```sh
theme 0-3          # four colour themes
wallpaper 0-5      # six wallpapers, the last a colour you choose
accent 4fc3f7      # the highlight colour every app uses (`accent off` reverts)
font 0-2           # interface scale: compact / normal / large
crt 0-1            # CRT scanline shader
customise          # print the current settings and how to change them
resetprefs         # back to defaults
cat /etc/desktop.cfg    # the settings are a plain text file
```

Two extra wallpapers ship with this: **Aurora Drift** (procedural curtains) and
**Solid Colour**, which fills the desktop with a colour of your choosing so the
machine can be made to look like nothing else.

### AE Browser

A real browser, in the machine, with an address bar, history, back/forward,
scrollable pages and clickable links. `browser` in the shell, or the globe icon.

It is a **reading view, not a rendering engine**: it fetches the page, discards
scripts and styling, and lays out the text, headings and links. Article-style
content -- documentation, wikis, news, search results -- renders very well;
JavaScript-heavy apps do not, and it says so rather than pretending.

Pages are fetched by the host process, because the wasm module is sandboxed.
The request is **asynchronous**: the host starts a `fetch()`, the kernel polls
it once per frame, and the window keeps repainting at full rate while it waits.
Blocking the module's thread would have frozen the window for the length of the
request, which is why `net_begin`/`net_poll`/`net_finish` exist alongside the
simple `net_fetch` used during boot.

Local pages (`ae://about`, `ae://files`, `ae://commands`) never
touch the network. On the Win32 build, which has no network stack, the browser
still runs and reports plainly that networking is unavailable.

## Verification

```sh
tools/syntax_check.sh    # warning-free compile of every module
tools/math_check.sh      # freestanding sqrt/sin/cos/pow/fmod/exp/log vs libm
tools/headless_run.sh /tmp/session.txt   # replay an input script, dump frames
tools/debug_build.sh -DAETHER_MM_CHECK -fsanitize=address,undefined
```

The headless chassis accepts a timestamped script (`mousemove`, `down`, `up`,
`click`, `key`, `wheel`, `dump <path.ppm>`, `quit`) and is how every UI change in
this project is checked without a Windows machine. It tracks button state across
`down`/`up`, so drags (paint strokes, window moves) can be scripted.

`AETHER_MM_CHECK` verifies the allocator's block chain and free list on every
allocation and panics on corruption; together with ASan it is how the
free-list merge bug and the over-the-edge framebuffer write were found.

> Note: a hand-run `gcc aether_os.c` compiles a *stale* concatenation -- use
> `tools/debug_build.sh`, which reassembles the modules first.

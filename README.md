# AetherOS

A complete 32-bit hobby operating system — kernel, virtual file system, process
scheduler, bytecode VM, window manager, and GUI applications — that boots inside
a single 337 KB Windows executable.

The whole OS is a freestanding C11 program. It links **no C runtime**: string,
memory, formatting, heap and floating-point routines are all implemented in the
kernel itself, so the built executable imports nothing but four Win32 system
libraries (`kernel32`, `user32`, `gdi32`, `winmm`).

## Build

```sh
tools/setup_toolchain.sh     # fetches zig 0.17.1 into ~/.local/zig (once)
tools/build.sh               # -> AetherOS.exe (Windows GUI app)
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
| `dev/85_app_sh.c` | AetherShell (command interpreter, `cc`, `run`) |
| `dev/86..93_app_*.c` | sysmon, explorer, editor, hexview, paint, settings, calc, console |
| `dev/95_main.c` | boot sequence, panic handler, main loop |

## Verification

```sh
tools/syntax_check.sh    # warning-free compile of every module
tools/math_check.sh      # freestanding sqrt/sin/cos/pow/fmod/exp/log vs libm
tools/headless_run.sh /tmp/session.txt   # replay an input script, dump frames
```

The headless chassis accepts a timestamped script (`mousemove`, `down`, `up`,
`click`, `key`, `wheel`, `dump <path.ppm>`, `quit`) and is how every UI change in
this project is checked without a Windows machine.

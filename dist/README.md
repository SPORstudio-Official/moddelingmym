# AE downloads

Two builds. Pick one.

---

## 1. `AE.html` — one click, no install  ✅ recommended

**1,302,455 bytes** — SHA-256 `b45779291be33b8fa76c1fde926f66dd2d968692e4b1ae68d96c08d09c9bee45`

Click the link, the file downloads, double-click it, and the OS is running. Works
on Windows, macOS and Linux, in any modern browser.

The whole operating system — kernel, compositor, scheduler, VFS, compiler and all
nine applications — is embedded in that one page as a WebAssembly module. There is
no separate `.wasm` to fetch, no server, no install, and **no network access at
runtime**. It runs offline from your Downloads folder.

- Boot animation → desktop in about 2.6 seconds
- Keyboard and mouse are fully wired: type `help` in AetherShell, click **Start**
- F11 for fullscreen, which makes it feel like a machine rather than a web page

> Browsers refuse to load a sibling `.wasm` from a `file://` page, which is why
> this build inlines everything. If you'd rather run the multi-file build, serve it
> over HTTP instead: see the main README's `npm run web`.

---

## 2. `AE.exe` — native Windows binary

**335,872 bytes** — SHA-256 `db51d14e75a3bd162e77142b3443e4ec65b9c061346aeead15a8e2c6796e2574`

Windows 10/11, x86-64. No installer: the OS, its disk image, its applications and
its fonts are all inside that single file. Double-click it.

On first launch Windows may show *"Windows protected your PC"* because the binary
is not code-signed — choose **More info → Run anyway**.

⚠️ **Known issue:** on some machines this shows no window and exits silently.
The GUI subsystem has no console, so a failure in early initialisation is
invisible. The HTML build above does not have this problem — it reports boot
failures on screen — so prefer it unless you specifically need the native binary.

### One-click install (alternative to double-clicking)

`install-ae.cmd` downloads the executable, verifies its checksum, places it
in `%LOCALAPPDATA%\AE` and starts it. Or run the PowerShell script directly
to get a Start Menu shortcut too:

```powershell
powershell -ExecutionPolicy Bypass -File install-ae.ps1 -AddShortcut
```

---

## Verify what you downloaded

```powershell
# Windows
Get-FileHash .\AE.html -Algorithm SHA256
```

```sh
# macOS / Linux
shasum -a 256 AE.html
```

The `.exe` imports only `kernel32`, `user32`, `gdi32` and `winmm` — all shipped
with Windows. No C runtime, no DLLs, no registry writes.

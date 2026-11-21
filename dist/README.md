# AE downloads

Two builds of the same operating system. Pick one.

---

## 1. `AE.exe` — the real thing  ✅ recommended

**821,760 bytes** — SHA-256 `99349ca3aec446e3742fe7efd30f61fdcbfd495b677a660f7d00f2af0e24a5cc`

Windows 10/11, x86-64. No installer, no browser, no dependencies: double-click it
and AE is running as a native program.

The file contains two executables. The outer one is a small launcher that owns the
icon, the Start-menu name and the taskbar identity, and writes the inner one to
`%LOCALAPPDATA%\AE\AE-0_2_0.exe`. The inner one *is* AE: its own window, its own
message loop, its own compositor, its own HTTP client, its own filesystem. Nothing
on the machine renders it for us.

- Boot animation → desktop in about 2.6 seconds
- Keyboard and mouse are fully wired: type `help` in Crommand, click **Start**
- It downloads pages itself, over HTTPS, with WinHTTP and the system certificate
  store — there is no browser process behind it
- Type words instead of an address in AE Browser and you get search results, from
  a free MCP server that needs no account (100 searches a day)

On first launch Windows may show *"Windows protected your PC"* because the binary
is not code-signed — choose **More info → Run anyway**.

### One-click install (alternative to double-clicking)

`install-ae.cmd` downloads the executable, verifies its checksum, places it in
`%LOCALAPPDATA%\AE` and starts it. Or run the PowerShell script directly to get a
Start Menu shortcut too:

```powershell
powershell -ExecutionPolicy Bypass -File install-ae.ps1 -AddShortcut
```

---

## 2. `AE.html` — the same OS in one page

**1,828,287 bytes** — SHA-256 `7f6a28d2fb710c1ecc40ea93faf610b011b409326d7d1438322206c8df10c843`

Click the link, the file downloads, double-click it, and the OS is running. Works
on Windows, macOS and Linux, in any modern browser.

The whole operating system — kernel, compositor, scheduler, VFS, compiler and all
ten applications — is embedded in that one page as a WebAssembly module. There is
no separate `.wasm` to fetch, no server, and no install. It runs offline from your
Downloads folder.

- F11 for fullscreen, which makes it feel like a machine rather than a web page
- Search needs the native build: a browser page cannot send the POST that an MCP
  server requires, so `AE.html` reports that plainly instead of failing silently

> Browsers refuse to load a sibling `.wasm` from a `file://` page, which is why
> this build inlines everything. If you'd rather run the multi-file build, serve it
> over HTTP instead: see the main README's `npm run web`.

---

## Verify what you downloaded

```powershell
# Windows
Get-FileHash .\AE.exe -Algorithm SHA256
```

```sh
# macOS / Linux
shasum -a 256 AE.html
```

`AE.exe` imports only `kernel32` and `advapi32`. The image it carries imports
`kernel32`, `user32`, `gdi32`, `shell32`, `winmm` and `winhttp` — all shipped with
Windows. No C runtime, no bundled DLLs.

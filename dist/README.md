# AetherOS downloads

**`AetherOS.exe`** — 335,872 bytes — SHA-256 `db51d14e75a3bd162e77142b3443e4ec65b9c061346aeead15a8e2c6796e2574`

Windows 10/11, x86-64. There is nothing to install: the OS, its disk image, its
applications and its fonts are all inside that single file. Double-click it.

On first launch Windows may show *"Windows protected your PC"* because the binary
is not code-signed — choose **More info → Run anyway**.

## One-click install

`install-aetheros.cmd` downloads the executable, verifies its checksum, places it
in `%LOCALAPPDATA%\AetherOS` and starts it. Or run the PowerShell script directly
to get a Start Menu shortcut too:

```powershell
powershell -ExecutionPolicy Bypass -File install-aetheros.ps1 -AddShortcut
```

## Verify what you downloaded

```powershell
Get-FileHash .\AetherOS.exe -Algorithm SHA256
```

The PE imports only `kernel32`, `user32`, `gdi32` and `winmm` — all shipped with
Windows. No C runtime, no DLLs, no registry writes.

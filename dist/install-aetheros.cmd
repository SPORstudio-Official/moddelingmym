@echo off
rem One-click launcher: fetches and runs AetherOS via the PowerShell installer.
rem Double-click this file, or run it from a terminal.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-aetheros.ps1" %*
if errorlevel 1 pause
endlocal

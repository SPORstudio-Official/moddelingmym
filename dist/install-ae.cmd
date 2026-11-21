@echo off
rem One-click launcher: fetches and runs AE via the PowerShell installer.
rem Double-click this file, or run it from a terminal.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-ae.ps1" %*
if errorlevel 1 pause
endlocal

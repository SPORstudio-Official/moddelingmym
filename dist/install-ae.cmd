@echo off
rem ---------------------------------------------------------------------------
rem  AE -- one-click launcher for the PowerShell installer.
rem
rem  Double-click this file. It runs the .ps1 next to it, which downloads the
rem  release, checks its SHA-256 and starts AE. If the .ps1 is not next to this
rem  file, it is fetched from the release.
rem
rem  -ExecutionPolicy Bypass is needed because a downloaded script is unsigned;
rem  it applies to this one invocation only and changes no machine setting.
rem ---------------------------------------------------------------------------
setlocal
set "HERE=%~dp0"
set "REL=https://github.com/SPORstudio-Official/moddelingmym/releases/download/v1.0.0-aether"

if exist "%HERE%install-ae.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%install-ae.ps1" %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "irm '%REL%/install-ae.ps1' | iex"
)

rem Keep the window open on failure so the message is readable: a console that
rem vanishes on an error is indistinguishable from nothing having happened.
if errorlevel 1 (
    echo.
    echo   Installation did not finish. The reason is printed above.
    echo.
    pause
)
endlocal

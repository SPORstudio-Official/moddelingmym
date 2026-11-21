@echo off
rem ---------------------------------------------------------------------------
rem  AE -- one-click launcher for the PowerShell installer.
rem
rem  Double-click this file. It runs the .ps1 next to it (or fetches it), which
rem  downloads AE, checks its SHA-256, installs it to %LOCALAPPDATA%\AE, adds a
rem  Start-menu shortcut and starts it.
rem
rem  -ExecutionPolicy Bypass is needed because a downloaded script is unsigned;
rem  it applies to this one invocation only and changes no machine setting.
rem ---------------------------------------------------------------------------
setlocal
set "HERE=%~dp0"
set "REL=https://github.com/SPORstudio-Official/moddelingmym/raw/main/dist"

rem -AddShortcut by default: someone who clicked "install" wants it in the
rem Start menu, not only running once.
if exist "%HERE%install-ae.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%install-ae.ps1" -AddShortcut %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "& ([scriptblock]::Create((irm '%REL%/install-ae.ps1'))) -AddShortcut"
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

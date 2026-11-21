<#
    AE -- one-click installer
    =========================

    Downloads AE, verifies its SHA-256, and starts it.  Optional Start-menu
    shortcut.

    One line, nothing to answer:

        powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/SPORstudio-Official/moddelingmym/releases/download/v1.0.0-aether/install-ae.ps1 | iex"

    Or download this file and right-click -> Run with PowerShell.

    There is genuinely nothing to install: AE is a single self-contained
    executable with no runtime, no DLLs and no registry footprint.  "Installing"
    therefore means "fetch it, check the checksum, start it".

    Why the checksum: the download is an executable, and this script is fetched
    over the network and run.  Pinning the hash in the script means a tampered
    or half-written download is refused instead of run.  When a new build ships,
    this file ships with it and the hash changes in step.
#>
[CmdletBinding()]
param(
    # Release asset name carries the version, so an already-installed copy and a
    # running copy are never in the way of each other.
    [string] $Version  = '1.0.0',
    [string] $BaseUrl  = 'https://github.com/SPORstudio-Official/moddelingmym/releases/download/v1.0.0-aether',
    [string] $Sha256   = '99349ca3aec446e3742fe7efd30f61fdcbfd495b677a660f7d00f2af0e24a5cc',
    [string] $InstallDir = (Join-Path $env:LOCALAPPDATA 'AE'),
    [switch] $AddShortcut,
    [switch] $NoLaunch
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # the progress bar makes downloads slower

function Write-Step($msg) { Write-Host "  $msg" }
function Fail($msg) {
    Write-Host ''
    Write-Host "  $msg" -ForegroundColor Red
    Write-Host ''
    exit 1
}

Write-Host ''
Write-Host '  AE' -ForegroundColor Cyan
Write-Host '  ------------------------------------------------------------' -ForegroundColor DarkCyan
Write-Host '  a 32-bit operating system in one executable' -ForegroundColor DarkGray
Write-Host ''

# --- 0. environment --------------------------------------------------------
if ([Environment]::OSVersion.Platform -ne 'Win32NT') {
    Fail 'This installer is for Windows. On macOS or Linux, open AE.html instead.'
}

# Windows PowerShell 5.1 defaults to TLS 1.0 on older builds, which GitHub
# rejects outright -- the symptom is a vague "underlying connection was closed".
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
} catch { }

$asset = "AE.exe"
$url   = "$BaseUrl/$asset"
$target = Join-Path $InstallDir 'AE.exe'
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

# --- 1. download -----------------------------------------------------------
Write-Step "downloading AE $Version"
$tmp = "$target.download"
try {
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
} catch {
    Write-Host ''
    Write-Host "  Download failed." -ForegroundColor Red
    Write-Host "    $url" -ForegroundColor DarkGray
    Write-Host "    $($_.Exception.Message)" -ForegroundColor DarkGray
    Write-Host ''
    Write-Host '  If you are signed out, sign in to GitHub first -- this repository' -ForegroundColor Yellow
    Write-Host '  is private, so the release asset is only served to accounts that' -ForegroundColor Yellow
    Write-Host '  can read it.' -ForegroundColor Yellow
    Write-Host ''
    exit 1
}

# --- 2. verify -------------------------------------------------------------
$actual = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash.ToLower()
if ($Sha256 -and $actual -ne $Sha256.ToLower()) {
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    Write-Host ''
    Write-Host '  Checksum mismatch -- the download was refused.' -ForegroundColor Red
    Write-Host "    expected $($Sha256.ToLower())" -ForegroundColor DarkGray
    Write-Host "    got      $actual" -ForegroundColor DarkGray
    Write-Host ''
    Write-Host '  Nothing was installed. Try running the installer again; if it keeps' -ForegroundColor Yellow
    Write-Host '  happening, the release has been updated and this script is stale.' -ForegroundColor Yellow
    Write-Host ''
    exit 1
}
Write-Step 'checksum ok'

# Mark of the Web: clear it, or SmartScreen warns about a file the user just
# chose to install and the shortcut collects a "blocked" flag.
try { Unblock-File -Path $tmp } catch { }

# A running copy holds its own image open, so replace-then-start is not enough:
# move the old one aside first and let the new file take the name.
if (Test-Path $target) {
    $old = "$target.old"
    Remove-Item $old -Force -ErrorAction SilentlyContinue
    try { Move-Item -Path $target -Destination $old -Force } catch { }
}
Move-Item -Path $tmp -Destination $target -Force
Write-Step "installed to $target"

# --- 3. optional shortcut --------------------------------------------------
if ($AddShortcut) {
    $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'AE.lnk'
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($lnk)
    $s.TargetPath       = $target
    $s.WorkingDirectory = $InstallDir
    $s.Description      = 'AE - a 32-bit operating system in one executable'
    $s.Save()
    Write-Step "shortcut created: $lnk"
}

# --- 4. launch -------------------------------------------------------------
if (-not $NoLaunch) {
    Write-Step 'starting AE'
    Start-Process -FilePath $target
}

Write-Host ''
Write-Host '  Done. AE is starting.' -ForegroundColor Green
Write-Host '  Run it again from the Start menu, or from:' -ForegroundColor DarkGray
Write-Host "    $target" -ForegroundColor DarkGray
Write-Host ''

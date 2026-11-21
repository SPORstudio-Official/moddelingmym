<#
    AE one-click installer
    ----------------------------
    Downloads the latest AE.exe, verifies its SHA-256, and launches it.

        powershell -ExecutionPolicy Bypass -File install-ae.ps1

    There is genuinely nothing to install: AE is a single self-contained
    executable with no runtime, no DLLs and no registry footprint.  "Installing"
    therefore means "fetch it, check the checksum, start it" -- which is what
    this script does.  The optional -AddShortcut switch drops a Start Menu
    shortcut so it can be launched again later.
#>
[CmdletBinding()]
param(
    [string] $Url = 'https://raw.githubusercontent.com/SPORstudio-Official/moddelingmym/main/dist/AE.exe',
    [string] $Sha256 = 'db51d14e75a3bd162e77142b3443e4ec65b9c061346aeead15a8e2c6796e2574',
    [string] $InstallDir = (Join-Path $env:LOCALAPPDATA 'AE'),
    [switch] $AddShortcut,
    [switch] $NoLaunch
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Write-Step($msg) { Write-Host "  $msg" }

Write-Host ''
Write-Host '  AE installer' -ForegroundColor Cyan
Write-Host '  -----------------' -ForegroundColor Cyan

# --- 1. download -----------------------------------------------------------
$target = Join-Path $InstallDir 'AE.exe'
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Write-Step "downloading $Url"
$tmp = "$target.download"
try {
    Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
} catch {
    Write-Host "  download failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host '  Tip: open the URL in a browser and save the file as AE.exe.' -ForegroundColor Yellow
    exit 1
}

# --- 2. verify -------------------------------------------------------------
$actual = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash.ToLower()
if ($Sha256 -and $actual -ne $Sha256.ToLower()) {
    Remove-Item $tmp -Force
    Write-Host "  checksum mismatch" -ForegroundColor Red
    Write-Host "    expected $($Sha256.ToLower())" -ForegroundColor DarkGray
    Write-Host "    got      $actual" -ForegroundColor DarkGray
    exit 1
}
Write-Step 'checksum ok'

# Unblock the Mark-of-the-Web so SmartScreen does not prompt a second time.
try { Unblock-File -Path $tmp } catch { }

Move-Item -Path $tmp -Destination $target -Force
Write-Step "installed to $target"

# --- 3. optional shortcut --------------------------------------------------
if ($AddShortcut) {
    $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'AE.lnk'
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($lnk)
    $s.TargetPath = $target
    $s.WorkingDirectory = $InstallDir
    $s.Description = 'AE - a 32-bit operating system in one executable'
    $s.Save()
    Write-Step "shortcut created: $lnk"
}

# --- 4. launch -------------------------------------------------------------
if (-not $NoLaunch) {
    Write-Step 'starting AE'
    Start-Process -FilePath $target
}

Write-Host ''
Write-Host '  Done.' -ForegroundColor Green
Write-Host ''

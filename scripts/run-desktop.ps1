# Launch the TaidaFlow desktop build (authoritative Core + mirror server) SAFELY.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 [-Label <text>]
#            [-Exe <path>] [-LogFile <path>] [-PvFile <path>] [-Wait]
#   -Label   : free text recorded with the safety probe (e.g. "E2E run 1")
#   -Exe     : default build\desktop\TaidaFlowApp.exe (baseline: build\baseline-desktop\...)
#   -LogFile : stderr log (default build\runtime-logs\desktop-<time>.log; stdout -> .stdout)
#   -PvFile  : enable the dev-only E2E PV driver (App/e2epvdriver.h) with this file
#   -Wait    : block until the app exits and return its exit code
#
# Safety (taidaflow WASM v4 spec §2):
#   1. scripts\safety_probe.ps1 runs first (TCP-connect probe of 192.168.1.201..205:502,
#      serial port list, 502/8123/8125 listeners). Any non-zero verdict -> the app is NOT
#      started and this script exits 3.
#   2. The working directory is ALWAYS build\runtime-cwd: the Core writes
#      TaidaFlowSettings.ini, device_info.ini and SQLite files (settings.sqlite, data\)
#      into the current directory; build\ is git-ignored, so the work tree stays clean.
param(
    [string]$Label = "manual",
    [string]$Exe = "",
    [string]$LogFile = "",
    [string]$PvFile = "",
    [switch]$Wait
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Exe -eq "") { $Exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe' }
if (-not [System.IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $root $Exe }
if (-not (Test-Path $Exe)) { Write-Output "not built: $Exe (run scripts\build-desktop.bat)"; exit 2 }
if (Get-Process TaidaFlowApp -ErrorAction SilentlyContinue) {
    Write-Output 'TaidaFlowApp is already running - close it first'; exit 2
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'safety_probe.ps1') -Reason "before launch: $Label ($Exe)"
if ($LASTEXITCODE -ne 0) {
    Write-Output "SAFETY PROBE verdict not SAFE (exit $LASTEXITCODE) - desktop app NOT started"
    exit 3
}

$cwd = Join-Path $root 'build\runtime-cwd'
New-Item -ItemType Directory -Force $cwd | Out-Null
if ($LogFile -eq "") {
    $LogFile = Join-Path $root ('build\runtime-logs\desktop-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
} elseif (-not [System.IO.Path]::IsPathRooted($LogFile)) {
    $LogFile = Join-Path $root $LogFile      # relative paths are relative to the taidaflow root
}
New-Item -ItemType Directory -Force (Split-Path -Parent $LogFile) | Out-Null

$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;" + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
if ($PvFile -ne "") {
    if (-not (Test-Path $PvFile)) { Set-Content -Path $PvFile -Value '#' -Encoding ascii }
    $env:TAIDAFLOW_E2E_PV_FILE = (Resolve-Path $PvFile).Path
} else {
    Remove-Item Env:\TAIDAFLOW_E2E_PV_FILE -ErrorAction SilentlyContinue
}

$p = Start-Process -FilePath $Exe -WorkingDirectory $cwd -PassThru `
        -RedirectStandardError $LogFile -RedirectStandardOutput "$LogFile.stdout"
$null = $p.Handle
Write-Output "PID=$($p.Id) CWD=$cwd LOG=$LogFile"
if ($Wait) { $p.WaitForExit(); exit $p.ExitCode }
exit 0

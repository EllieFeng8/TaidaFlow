# Launch the TaidaFlow desktop build (authoritative Core + mirror server) SAFELY.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 [-Label <text>]
#            [-Exe <path>] [-LogFile <path>] [-Wait]
#   -Label   : free text recorded with the safety probe (e.g. "E2E run 1")
#   -Exe     : default build\desktop\TaidaFlowApp.exe
#   -LogFile : stderr log (default build\runtime-logs\desktop-<time>.log; stdout -> .stdout)
#   -ProbeLog: safety-probe log file (default docs\evidence\wasm-v4\safety-probe.log)
#   -Wait    : block until the app exits and return its exit code
#   -DeviceProfile simulator : TEST ONLY. Runs the probe in simulator mode (502 may only be
#              held by Adam60xxSimulator.exe on 127.0.0.201..205) and starts the app with
#              TAIDAFLOW_DEVICE_PROFILE=simulator (ADAM sessions -> 127.0.0.201..205:502).
#              Default probe log: docs\evidence\wasm-v4-sim\.
#              Without it (default) TAIDAFLOW_DEVICE_PROFILE is removed from the app's
#              environment, so the app always uses the plant addresses the probe checked.
#
# Safety (taidaflow WASM v4 spec §2):
#   1. scripts\safety_probe.ps1 runs first (TCP-connect probe of 192.168.1.201..205:502,
#      serial port list, 502/8124/8125/18125 and REST port (w2-060, TAIDAFLOW_REST_PORT or 18080)
#      listeners). Any non-zero verdict -> the app is NOT
#      started and this script exits 3.
#   2. The working directory is ALWAYS build\runtime-cwd: the Core writes
#      TaidaFlowSettings.ini, device_info.ini and SQLite files (settings.sqlite, data\)
#      into the current directory; build\ is git-ignored, so the work tree stays clean.
#   The environment (TAIDAFLOW_DOWNLOAD_PORT, TAIDAFLOW_REST_PORT, ...) is passed to the app as it is.
param(
    [string]$Label = "manual",
    [string]$Exe = "",
    [string]$LogFile = "",
    [string]$ProbeLog = "",
    [switch]$Wait,
    [ValidateSet('default', 'simulator')]
    [string]$DeviceProfile = "default"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Exe -eq "") { $Exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe' }
if (-not [System.IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $root $Exe }
if (-not (Test-Path $Exe)) { Write-Output "not built: $Exe (run scripts\build-desktop.bat)"; exit 2 }
if (Get-Process TaidaFlowApp -ErrorAction SilentlyContinue) {
    Write-Output 'TaidaFlowApp is already running - close it first'; exit 2
}

$probeArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'safety_probe.ps1'),
               '-Reason', "before launch: $Label ($Exe)")
if ($DeviceProfile -eq 'simulator') { $probeArgs += @('-DeviceProfile', 'simulator') }
if ($ProbeLog -ne "") {
    if (-not [System.IO.Path]::IsPathRooted($ProbeLog)) { $ProbeLog = Join-Path $root $ProbeLog }
    $probeArgs += @('-LogFile', $ProbeLog)
}
& powershell @probeArgs
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
# w2-060: the development app always runs in build\runtime-cwd: a TAIDAFLOW_DATA_DIR of this shell
# would move it (the app prefers it to config.json; build\desktop has no config.json).
Remove-Item Env:\TAIDAFLOW_DATA_DIR -ErrorAction SilentlyContinue
if ($DeviceProfile -eq 'simulator') {
    $env:TAIDAFLOW_DEVICE_PROFILE = 'simulator'
} else {
    Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue
}

$p = Start-Process -FilePath $Exe -WorkingDirectory $cwd -PassThru `
        -RedirectStandardError $LogFile -RedirectStandardOutput "$LogFile.stdout"
$null = $p.Handle
Write-Output "PID=$($p.Id) CWD=$cwd LOG=$LogFile$(if ($DeviceProfile -eq 'simulator') { ' PROFILE=simulator' })"
if ($Wait) { $p.WaitForExit(); exit $p.ExitCode }
exit 0

# Launch the TaidaFlow desktop build (authoritative Core + mirror server) SAFELY on a development PC.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\run-desktop.ps1 [-Label <text>]
#            [-Config <config.json>] [-Exe <path>] [-LogFile <path>] [-ProbeLog <path>] [-Wait]
#            [-DeviceProfile default|simulator]
#   -Label   : free text recorded with the safety probe (e.g. "E2E run 1")
#   -Config  : w2-062: the config.json of this run (default deploy\dev\config.dev.json; e.g.
#              deploy\dev\config.simulator.json = devices directly on 127.0.0.201..205). The app gets
#              it as TAIDAFLOW_CONFIG, so it NEVER creates a config.json next to build\desktop
#              (that file would hold the plant defaults: data in C:\TaidaFlowData, plant IP
#              addresses). The file must exist (this script refuses a missing one).
#   -Exe     : default build\desktop\TaidaFlowApp.exe
#   -LogFile : stderr log (default build\runtime-logs\desktop-<time>.log; stdout -> .stdout)
#   -ProbeLog: safety-probe log file (default build\runtime-logs\safety-probe[-sim].log)
#   -Wait    : block until the app exits and return its exit code
#   -DeviceProfile simulator : TEST ONLY. Runs the probe in simulator mode and starts the app with
#              TAIDAFLOW_DEVICE_PROFILE=simulator (ADAM sessions -> 127.0.0.201..205, port and unit
#              id from config.json). Without it TAIDAFLOW_DEVICE_PROFILE is removed from the app's
#              environment, so the app always uses the addresses the probe checked.
#
# Safety (taidaflow WASM v4 spec section 2):
#   1. scripts\safety_probe.ps1 runs first with the SAME config.json (effective device addresses,
#      MS300 serial port, service ports of config.json). Any non-zero verdict -> the app is NOT
#      started and this script exits 3.
#   2. The working directory is the config's dataDir (config.dev.json: build\runtime-cwd); the app
#      switches to it itself (AppConfig::applyDataDir) and writes TaidaFlowSettings.ini,
#      device_info.ini and the SQLite files there; build\ is git-ignored.
#   The environment (TAIDAFLOW_DOWNLOAD_PORT, ...) is passed to the app as it is; TAIDAFLOW_CONFIG is
#   always set to -Config.
# Exit codes: 0 started; 2 not built / already running / config missing or unusable; 3 probe not SAFE.
param(
    [string]$Label = "manual",
    [string]$Config = "",
    [string]$Exe = "",
    [string]$LogFile = "",
    [string]$ProbeLog = "",
    [switch]$Wait,
    [ValidateSet('default', 'simulator')]
    [string]$DeviceProfile = "default"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
if ($Exe -eq "") { $Exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe' }
if (-not [System.IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $root $Exe }
if (-not (Test-Path $Exe)) { Write-Output "not built: $Exe (run scripts\build-desktop.bat)"; exit 2 }
if (Get-Process TaidaFlowApp -ErrorAction SilentlyContinue) {
    Write-Output 'TaidaFlowApp is already running - close it first'; exit 2
}
if ($Config -eq "") { $Config = Join-Path $root 'deploy\dev\config.dev.json' }
elseif (-not [System.IO.Path]::IsPathRooted($Config)) { $Config = Join-Path $root $Config }
$Config = [System.IO.Path]::GetFullPath($Config)
if (-not (Test-Path -LiteralPath $Config -PathType Leaf)) {
    Write-Output "config not found: $Config - refused (the app would create one with the plant defaults there)"
    exit 2
}
$besideExe = Join-Path (Split-Path -Parent $Exe) 'config.json'
if (Test-Path -LiteralPath $besideExe) {
    Write-Output "NOTE: $besideExe exists but is NOT used (TAIDAFLOW_CONFIG=$Config wins); a development build folder should not hold a config.json - delete it"
}
$cfg = Get-TaidaFlowConfig -Path $Config -Exe $Exe
if ($cfg.Error) { Write-Output "config unusable: $($cfg.Error)"; exit 2 }

$probeArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'safety_probe.ps1'),
               '-Reason', "before launch: $Label ($Exe)", '-Config', $Config, '-Exe', $Exe)
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

$cwd = $cfg.DataDir
New-Item -ItemType Directory -Force $cwd | Out-Null
if ($LogFile -eq "") {
    $LogFile = Join-Path $root ('build\runtime-logs\desktop-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
} elseif (-not [System.IO.Path]::IsPathRooted($LogFile)) {
    $LogFile = Join-Path $root $LogFile      # relative paths are relative to the taidaflow root
}
New-Item -ItemType Directory -Force (Split-Path -Parent $LogFile) | Out-Null

$qtRoot = if ($env:TAIDAFLOW_QT_ROOT) { $env:TAIDAFLOW_QT_ROOT } else { 'C:\Qt\6.8.3' }
$env:PATH = (Join-Path $qtRoot 'msvc2022_64\bin') + ';' + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_CONFIG = $Config
if ($DeviceProfile -eq 'simulator') {
    $env:TAIDAFLOW_DEVICE_PROFILE = 'simulator'
} else {
    Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue
}

$p = Start-Process -FilePath $Exe -WorkingDirectory $cwd -PassThru `
        -RedirectStandardError $LogFile -RedirectStandardOutput "$LogFile.stdout"
$null = $p.Handle
Write-Output "PID=$($p.Id) CONFIG=$Config DATADIR=$cwd LOG=$LogFile$(if ($DeviceProfile -eq 'simulator') { ' PROFILE=simulator' })"
if ($Wait) { $p.WaitForExit(); exit $p.ExitCode }
exit 0

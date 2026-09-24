# Start Adam60xxSimulator (separate project, used READ-ONLY: only its already-built exe is
# executed; nothing is built or written inside its folder) for the TEST-ONLY simulator
# profile of the desktop app (TAIDAFLOW_DEVICE_PROFILE=simulator, see run-desktop.ps1).
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\run-simulator.ps1 [-SimulatorExe <path>]
#            [-LogFile <path>] [-TimeoutSec 20]
#   -SimulatorExe : default ..\Adam60xxSimulator\build\Adam60xxSimulator.exe (next to taidaflow\)
#   -LogFile      : stderr log (default build\runtime-logs\simulator-<time>.log; stdout -> .stdout)
#
# * Working directory is ALWAYS taidaflow\build\sim-cwd (git-ignored).
# * Refuses to start (exit 3) when anything already listens on port 502, and (exit 2) when an
#   Adam60xxSimulator process is already running - it is never stopped by this script.
# * Starts with --autostart (five ADAM servers on 127.0.0.201..205:502, unit 1) and waits
#   until the five endpoints are listening and owned by the new process.
# Exit 0 = five endpoints listening (prints PID/CWD/LOG); 1 = not listening in time (the
# started process is closed again); 2/3 = refused.
param(
    [string]$SimulatorExe = "",
    [string]$LogFile = "",
    [int]$TimeoutSec = 20
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($SimulatorExe -eq "") {
    $SimulatorExe = Join-Path (Split-Path -Parent $root) 'Adam60xxSimulator\build\Adam60xxSimulator.exe'
}
if (-not (Test-Path $SimulatorExe)) { Write-Output "simulator exe not found: $SimulatorExe"; exit 2 }
$running = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($running.Count) {
    Write-Output ("Adam60xxSimulator already running (pid " + (($running | ForEach-Object { $_.Id }) -join ', ') + ") - not started, not stopped")
    exit 2
}
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -eq 502 })
if ($busy.Count) {
    foreach ($l in $busy) {
        $pname = try { (Get-Process -Id $l.OwningProcess -ErrorAction Stop).ProcessName } catch { '?' }
        Write-Output ("port 502 already in use: {0}:{1} pid={2} ({3}) - simulator NOT started (owner not stopped)" -f $l.LocalAddress, $l.LocalPort, $l.OwningProcess, $pname)
    }
    exit 3
}

$cwd = Join-Path $root 'build\sim-cwd'
New-Item -ItemType Directory -Force $cwd | Out-Null
if ($LogFile -eq "") {
    $LogFile = Join-Path $root ('build\runtime-logs\simulator-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
} elseif (-not [System.IO.Path]::IsPathRooted($LogFile)) {
    $LogFile = Join-Path $root $LogFile
}
New-Item -ItemType Directory -Force (Split-Path -Parent $LogFile) | Out-Null

$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;" + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$p = Start-Process -FilePath $SimulatorExe -ArgumentList '--autostart' -WorkingDirectory $cwd -PassThru `
        -RedirectStandardError $LogFile -RedirectStandardOutput "$LogFile.stdout"
$null = $p.Handle

$hosts = @(201..205 | ForEach-Object { "127.0.0.$_" })
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$up = @()
do {
    Start-Sleep -Milliseconds 500
    $up = @(Get-NetTCPConnection -State Listen -LocalPort 502 -ErrorAction SilentlyContinue |
            Where-Object { $_.OwningProcess -eq $p.Id -and $hosts -contains $_.LocalAddress } |
            ForEach-Object { $_.LocalAddress } | Sort-Object -Unique)
} while ($up.Count -lt 5 -and -not $p.HasExited -and (Get-Date) -lt $deadline)

if ($up.Count -lt 5) {
    Write-Output ("simulator endpoints listening: {0}/5 after {1}s - closing it (exited={2})" -f $up.Count, $TimeoutSec, $p.HasExited)
    if (-not $p.HasExited) {
        $null = $p.CloseMainWindow()
        if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force }
    }
    exit 1
}
Write-Output "SIM_PID=$($p.Id) CWD=$cwd LOG=$LogFile endpoints=$($up -join ',')"
exit 0

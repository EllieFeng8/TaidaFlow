# TaidaFlow - FIELD (production machine) stop script.                (w2-057, w2-062)
# Counterpart of start-taidaflow.ps1 (same installation folder, same config.json).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File stop-taidaflow.ps1
#            [-Config <config.json>] [-DataDir <folder>] [-LogDir <folder>] [-TimeoutSec 60] [-Force]
#   w2-062: the data folder is config.json dataDir (default <installation folder>\config.json, or
#   -Config); its taidaflow-app.json identifies the app. config.json is only read, never created.
#   -DataDir : only when start-taidaflow.ps1 was started with the same one-time -DataDir.
#
# What it does:
#   * nginx is NOT stopped (w2-062, Mango A6: nginx is independent of the app; stop it by hand with
#     "cd <nginx folder>" + "nginx -s quit"). A running nginx is only reported.
#   * TaidaFlowApp - only the process recorded in <DataDir>\taidaflow-app.json (pid + image path +
#      start time must match). It is closed like a user closing the window (WM_CLOSE to its main
#      window), so the app runs its normal shutdown (HTTP service stop, SQLite close). The script
#      waits up to -TimeoutSec seconds.
#      If the app does not exit in time the script only REPORTS it (exit 7) and does not kill it.
#      -Force: kill it after the graceful attempt failed. RISK: the app gets no chance to finish
#      its shutdown - the last SQLite write or a CSV export in progress can be lost or left
#      incomplete, and the Modbus outputs (pumps, valves) simply keep the last written state.
#      Use -Force only when the window cannot be closed normally.
#   Other TaidaFlowApp processes (not started by start-taidaflow.ps1 with this data folder) are
#   listed, never touched.
#
# Exit codes: 0 stopped; 1 no app started by start-taidaflow.ps1 (with this data folder) is
# running; 7 the app did not exit (not killed; or kill failed with -Force);
# 2 config.json unusable (pass -DataDir). (9 = nginx stop failure is no longer used: nginx is not stopped.)
param(
    [string]$Config = "",
    [string]$DataDir = "",
    [string]$LogDir = "",
    [int]$TimeoutSec = 60,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$install = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $install $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
$configNote = ''
if ($DataDir -eq "") {
    # config.json dataDir (the defaults of TaidaFlowApp.exe for keys missing in the file).
    . (Join-Path $install 'scripts\taidaflow-config.ps1')
    $configPath = if ($Config -ne "") { Full $Config } else { Join-Path $install 'config.json' }
    $cfg = Get-TaidaFlowConfig -Path $configPath -Exe (Join-Path $install 'TaidaFlowApp.exe')
    if ($cfg.Error) { [Console]::Out.WriteLine("ERROR: $($cfg.Error) - pass -DataDir <data folder>"); exit 2 }
    $DataDir = $cfg.DataDir
    $configNote = " from $configPath dataDir ($($cfg.Sources['dataDir']))"
}
$DataDir = Full $DataDir
if ($LogDir -eq "") { $LogDir = Join-Path $DataDir 'logs' }
$LogDir = Full $LogDir
$stateFile = Join-Path $DataDir 'taidaflow-app.json'

$script:launcherLog = $null
if (Test-Path $LogDir -PathType Container) { $script:launcherLog = Join-Path $LogDir 'launcher.log' }
function Log([string]$m) {
    $line = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + ' [stop]  ' + $m
    [Console]::Out.WriteLine($line)
    if ($script:launcherLog) {
        try { [System.IO.File]::AppendAllText($script:launcherLog, $line + "`r`n", (New-Object System.Text.UTF8Encoding($false))) } catch { }
    }
}
function SamePath([string]$a, [string]$b) { return [string]::Equals($a, $b, [System.StringComparison]::OrdinalIgnoreCase) }

Log "=== TaidaFlow stop (data folder $DataDir$configNote) ==="
$state = $null
if (Test-Path $stateFile -PathType Leaf) {
    try { $state = Get-Content -Raw -LiteralPath $stateFile | ConvertFrom-Json } catch { Log "state file unreadable: $stateFile"; $state = $null }
}
# --- nginx: NOT stopped (w2-062, Mango A6) -------------------------------------------------------
# nginx is independent of the app (started with "start nginx" in its folder, by hand or by
# start-taidaflow.ps1). Stop it by hand when needed:  cd <nginx folder>  then  nginx -s quit
$nginxFailed = $false
$running = @(Get-Process nginx -ErrorAction SilentlyContinue)
if ($running.Count) { Log ("nginx keeps running (pid {0}; stop it with: cd <nginx folder> ; nginx -s quit)" -f (($running | ForEach-Object { $_.Id }) -join ', ')) }

# --- the app (ours only) -------------------------------------------------------------------------
$ours = $null
if ($state -and $state.pid) {
    $p = Get-Process -Id ([int]$state.pid) -ErrorAction SilentlyContinue
    if ($p) {
        $path = try { $p.Path } catch { '' }
        $ticks = try { $p.StartTime.ToUniversalTime().Ticks } catch { 0 }
        if ((SamePath $path ([string]$state.exe)) -and [int64]$ticks -eq [int64]$state.startTicksUtc) { $ours = $p }
        else { Log "pid $($state.pid) is now another process ($path) - not ours, not touched" }
    }
}
foreach ($o in @(Get-Process -Name TaidaFlowApp -ErrorAction SilentlyContinue)) {
    if (-not $ours -or $o.Id -ne $ours.Id) {
        $path = try { $o.Path } catch { '?' }
        Log "other TaidaFlowApp (not started by start-taidaflow.ps1 with this data folder, not touched): pid $($o.Id) $path"
    }
}
if (-not $ours) {
    Log "no TaidaFlowApp started by start-taidaflow.ps1 (data folder $DataDir) is running"
    if (Test-Path $stateFile) { Remove-Item -LiteralPath $stateFile -Force; Log "stale state file removed" }
    if ($nginxFailed) { exit 9 }
    exit 1
}
$null = $ours.Handle
$ours.Refresh()
$appPid = $ours.Id
if ($ours.MainWindowHandle -eq [IntPtr]::Zero) {
    Log "WARNING: pid $appPid has no main window in this session (started by another user / session?) - cannot close it gracefully from here"
} else {
    Log "closing TaidaFlowApp pid $appPid (WM_CLOSE to its window, like closing it by hand)"
    $null = $ours.CloseMainWindow()
}
$exited = $ours.WaitForExit($TimeoutSec * 1000)
if (-not $exited) {
    if (-not $Force) {
        Log "ERROR: TaidaFlowApp pid $appPid did not exit within $TimeoutSec s - NOT killed. Close the window by hand, or run again with -Force (see the risk in this script's header)"
        exit 7
    }
    Log "-Force: killing TaidaFlowApp pid $appPid (no normal shutdown: last SQLite write / running CSV export may be lost; outputs keep their last state)"
    Stop-Process -Id $appPid -Force
    if (-not $ours.WaitForExit(15000)) { Log "ERROR: kill failed, pid $appPid still running"; exit 7 }
}
$code = try { $ours.ExitCode } catch { '?' }
Log "TaidaFlowApp pid $appPid exited (exit code $code)"
Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
$left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $appPid })
Log ("listeners of pid {0} left: {1}" -f $appPid, $left.Count)
if ($nginxFailed) { exit 9 }
Log "stopped (exit 0)"
exit 0

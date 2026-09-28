# w2-065 triage (DEVELOPMENT PC, simulator only): is the exit code 0xC0000005 seen after WM_CLOSE in the live run
# caused by the new start method (no console window, broken stdout/stderr pipes) or by the app itself?
# Runs build\desktop\TaidaFlowApp.exe the OLD development way (scripts\run-desktop.ps1: Start-Process with
# stderr/stdout redirected to files, QT_FORCE_STDERR_LOGGING=1, its own console) with the live-run config
# (build\w2-065-run\config.json, simulator addresses), waits -RunSec, closes the window (CloseMainWindow) and
# records the exit code. Repeats -Runs times. Simulator started / closed here; nothing else is touched.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-065\tools\shutdown-triage.ps1 [-Runs 2] [-RunSec 15] [-NoSimulator]
param([int]$Runs = 2, [int]$RunSec = 15, [switch]$NoSimulator, [string]$Exe = "", [string]$RunTag = "", [string]$ScriptsRoot = "")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$ev = Join-Path $root 'docs\evidence\w2-065\62-shutdown-triage'
New-Item -ItemType Directory -Force $ev | Out-Null
$cfg = Join-Path $root 'build\w2-065-run\config.json'
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$tag = if ($NoSimulator) { 'nosim' } else { 'sim' }
# -ScriptsRoot: use scripts\run-desktop.ps1 of another source tree (e.g. an older commit exported with git archive,
# whose scripts match its exe); default this repository.
$scriptsFrom = if ($ScriptsRoot -ne '') { [System.IO.Path]::GetFullPath($ScriptsRoot) } else { $root }
if ($RunTag -ne '') { $tag = "$RunTag-$tag" }
# -Exe: another TaidaFlowApp.exe (e.g. a build of an older commit); default build\desktop\TaidaFlowApp.exe (run-desktop's default)
$exeArg = if ($Exe -ne '') { ' -Exe "' + [System.IO.Path]::GetFullPath($Exe) + '"' } else { '' }
$lines = New-Object System.Collections.Generic.List[string]
function Note([string]$m) { $lines.Add($m); [Console]::Out.WriteLine($m) }
if (@(Get-Process | Where-Object { $_.ProcessName -in 'TaidaFlowApp', 'Adam60xxSimulator' }).Count) { Note 'TaidaFlowApp / simulator already running - nothing started'; exit 3 }
$simPid = 0
if (-not $NoSimulator) {
    $simCon = Join-Path $ev 'run-simulator.console.txt'
    $sp = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -WindowStyle Hidden -PassThru -ArgumentList ('/c ""' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $root 'scripts\run-simulator.ps1') + '" > "' + $simCon + '" 2>&1"')
    $null = $sp.Handle; $null = $sp.WaitForExit(60000)
    $t = (Get-Content $simCon) -join ' '
    if ($t -match 'SIM_PID=(\d+)') { $simPid = [int]$Matches[1] } else { Note "simulator not started: $t"; exit 3 }
    Note "simulator pid $simPid"
}
try {
    for ($i = 1; $i -le $Runs; $i++) {
        $con = Join-Path $ev "$tag-run$i-run-desktop.console.txt"
        $logFile = Join-Path $root "build\w2-065-run\triage-$tag-run$i.stderr.log"
        $rp = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -WindowStyle Hidden -PassThru -ArgumentList ('/c ""' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $scriptsFrom 'scripts\run-desktop.ps1') + '" -Label "w2-065 shutdown triage" -Config "' + $cfg + '" -LogFile "' + $logFile + '" -ProbeLog "' + (Join-Path $ev 'safety-probe.log') + '"' + $exeArg + ' > "' + $con + '" 2>&1"')
        $null = $rp.Handle; $null = $rp.WaitForExit(120000)
        $out = (Get-Content $con) -join ' '
        # case-sensitive and anchored: the probe output also contains "pid=<simulator pid>"
        if ($out -cnotmatch '\bPID=(\d+) CONFIG=') { Note "run $i : app not started ($out)"; break }
        $app = Get-Process -Id ([int]$Matches[1])
        if ($app.ProcessName -ne 'TaidaFlowApp') { Note "run $i : pid $($app.Id) is $($app.ProcessName), not TaidaFlowApp - stopped here"; break }
        $null = $app.Handle
        Start-Sleep -Seconds $RunSec
        $app.Refresh()
        $sent = if ($app.MainWindowHandle -ne [IntPtr]::Zero) { $app.CloseMainWindow() } else { $false }
        $exited = $app.WaitForExit(30000)
        if (-not $exited) { Stop-Process -Id $app.Id -Force; $null = $app.WaitForExit(10000) }
        $code = $app.ExitCode
        Note ("{0} run {1}: run-desktop (redirected stderr, console, QT_FORCE_STDERR_LOGGING=1), exe $(try { $app.Path } catch { '?' }), {2} s, WM_CLOSE sent {3}, exited {4}, exit code {5} (0x{6:X8})" -f $tag, $i, $RunSec, $sent, $exited, $code, $code)
        Copy-Item -LiteralPath $logFile -Destination (Join-Path $ev "$tag-run$i.stderr.log") -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
} finally {
    if ($simPid) {
        $s = Get-Process -Id $simPid -ErrorAction SilentlyContinue
        if ($s) { $null = $s.CloseMainWindow(); if (-not $s.WaitForExit(10000)) { Stop-Process -Id $simPid -Force }; Note "simulator pid $simPid closed" }
    }
    [System.IO.File]::AppendAllLines((Join-Path $ev 'summary.txt'), [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}

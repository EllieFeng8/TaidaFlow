# w2-067 shutdown test (DEVELOPMENT PC, Adam60xxSimulator only).
# Starts the simulator (unless -NoSimulator), then -Runs times: safety probe (must say SAFE, exit 0) ->
# start TaidaFlowApp.exe (config build\w2-067-run\config.json: devices = simulator 127.0.0.201..205,
# Modbus server port 15020, dataDir build\w2-067-run\data) -> wait -RunSec -> WM_CLOSE (CloseMainWindow)
# -> record the exit code and the stderr tail. The simulator (started here) is closed at the end.
# Nothing that this script did not start is stopped; it refuses to start when TaidaFlowApp,
# Adam60xxSimulator or cdb are already running.
# -Debugger: attach cdb.exe (Windows SDK debugger, already installed) after -RunSec, local symbols only
#            (exe folder + Qt bin); on an access violation cdb writes the faulting stack and every thread's
#            stack to <tag>-runN.cdb.txt and ends the process (exit code 0xC0000354 then).
# -NoSimulator: deploy\dev\config.dev.json (plant addresses, unreachable here; the probe checks) instead.
# After simulator runs: build\w2-067-dbcheck\w2067_dbcheck.exe (tools\build-dbcheck.bat) checks the monthly
# data file: SQLite integrity_check ok and the newest sensor_data row at most 3 s before each WM_CLOSE.
# The app is started through "cmd /c" (stderr/stdout to files) with CreateNoWindow: no console window,
# the QML window itself is shown normally so that WM_CLOSE reaches it (no UI interaction, no screenshots).
# Output: docs\evidence\w2-067\shutdown-<tag>\ (summary.txt, per-run stderr, safety-probe.log).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-067\tools\shutdown-test.ps1
#            [-Exe build\desktop\TaidaFlowApp.exe] [-Runs 10] [-RunSec 15] [-NoSimulator] [-Debugger] [-Tag name]
# Exit code: 0 = every run exited with code 0; 1 = at least one run failed; 3 = refused / not started.
param([string]$Exe = "", [int]$Runs = 10, [int]$RunSec = 15, [switch]$NoSimulator, [switch]$Debugger,
      [string]$Tag = "", [string]$OutDir = "")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
if ($Exe -eq "") { $Exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe' }
if (-not [System.IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $root $Exe }
$Exe = [System.IO.Path]::GetFullPath($Exe)
if ($Tag -eq "") { $Tag = if ($NoSimulator) { 'nosim' } else { 'sim' } }
if ($OutDir -eq "") { $OutDir = Join-Path $root "docs\evidence\w2-067\shutdown-$Tag" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$runDir = Join-Path $root 'build\w2-067-run'
$dataDir = Join-Path $runDir 'data'
New-Item -ItemType Directory -Force $dataDir | Out-Null
$cfg = Join-Path $runDir 'config.json'
# Simulator config (deploy\dev\config.simulator.json) with its own dataDir and Modbus server port 15020
# (the simulator itself listens on 127.0.0.201..205:502). -NoSimulator: deploy\dev\config.dev.json (plant
# addresses 192.168.1.201..205, not reachable from the development PC - the safety probe checks that; no
# device connected) with the same dataDir / server port.
$cfgBase = if ($NoSimulator) { 'deploy\dev\config.dev.json' } else { 'deploy\dev\config.simulator.json' }
$cfgText = (Get-Content -Raw -LiteralPath (Join-Path $root $cfgBase))
$cfgText = $cfgText -replace '"dataDir"\s*:\s*"[^"]*"', '"dataDir": "data"'
$cfgText = $cfgText -replace '("modbusServer"\s*:\s*\{[^}]*"port"\s*:\s*)502', '${1}15020'
[System.IO.File]::WriteAllText($cfg, $cfgText, (New-Object System.Text.UTF8Encoding($false)))
Add-Type -Namespace W2067 -Name User32 -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);'
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
$summary = Join-Path $OutDir 'summary.txt'
$lines = New-Object System.Collections.Generic.List[string]
function Note([string]$m) { $lines.Add($m); [Console]::Out.WriteLine($m) }
Note ("=== w2-067 shutdown test {0} tag={1} exe={2} runs={3} runSec={4} simulator={5} debugger={6}" -f (Get-Date -Format o), $Tag, $Exe, $Runs, $RunSec, (-not $NoSimulator), [bool]$Debugger)
if (-not (Test-Path -LiteralPath $Exe)) { Note "exe not found"; exit 3 }
$busy = @(Get-Process | Where-Object { $_.ProcessName -in 'TaidaFlowApp', 'Adam60xxSimulator', 'cdb' })
if ($busy.Count) { Note ("already running: " + (($busy | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ') + " - nothing started"); [System.IO.File]::AppendAllLines($summary, [string[]]$lines); exit 3 }
$simPid = 0
$failed = 0
$done = 0
$closes = New-Object System.Collections.Generic.List[long]
try {
    if (-not $NoSimulator) {
        $simCon = Join-Path $OutDir 'run-simulator.console.txt'
        $sp = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -WindowStyle Hidden -PassThru -ArgumentList ('/c ""' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $root 'scripts\run-simulator.ps1') + '" -LogFile "' + (Join-Path $runDir 'simulator.log') + '" > "' + $simCon + '" 2>&1"')
        $null = $sp.Handle; $null = $sp.WaitForExit(60000)
        $t = (Get-Content $simCon) -join ' '
        if ($t -cmatch '\bSIM_PID=(\d+)') { $simPid = [int]$Matches[1] } else { Note "simulator not started: $t"; exit 3 }
        Note "simulator pid $simPid ($t)"
    }
    $env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
    $env:TAIDAFLOW_CONFIG = $cfg
    $env:QT_FORCE_STDERR_LOGGING = '1'
    Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue
    for ($i = 1; $i -le $Runs; $i++) {
        $probeLog = Join-Path $OutDir 'safety-probe.log'
        $probeOut = & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\safety_probe.ps1') -Reason "w2-067 shutdown test $Tag run $i" -Config $cfg -Exe $Exe -LogFile $probeLog 2>&1
        $probeCode = $LASTEXITCODE
        $verdict = (($probeOut | Out-String) -split "`r?`n" | Where-Object { $_ -match 'verdict:' } | Select-Object -Last 1)
        if ($probeCode -ne 0) { Note "run $i : safety probe exit $probeCode ($verdict) - not started, test stopped"; $failed++; break }
        $errLog = Join-Path $OutDir "$Tag-run$i.stderr.log"
        $outLog = Join-Path $OutDir "$Tag-run$i.stdout.log"
        $started = Get-Date
        # cmd /c only redirects stderr/stdout to files; CreateNoWindow = no console window, the app window
        # itself is shown normally (a hidden start would hide the QML window: no main window for WM_CLOSE).
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = (Join-Path $env:SystemRoot 'System32\cmd.exe')
        $psi.Arguments = '/c ""' + $Exe + '" 2>"' + $errLog + '" 1>"' + $outLog + '""'
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.WorkingDirectory = $dataDir
        $wrapper = [System.Diagnostics.Process]::Start($psi)
        $app = $null
        for ($w = 0; $w -lt 40 -and -not $app; $w++) {
            Start-Sleep -Milliseconds 250
            $c = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($wrapper.Id) AND Name='TaidaFlowApp.exe'"
            if ($c) { $app = Get-Process -Id ([int]@($c)[0].ProcessId) }
        }
        if (-not $app) { Note "run $i : app not started"; $failed++; break }
        $null = $app.Handle
        Note ("run {0} : probe {1}; app pid {2} started {3}" -f $i, $verdict.Trim(), $app.Id, $started.ToString('HH:mm:ss'))
        Start-Sleep -Seconds $RunSec
        $app.Refresh()
        $hwnd = $app.MainWindowHandle   # taken before a debugger attaches
        $host_ = $null
        $sym = (Split-Path -Parent $Exe) + ';C:\Qt\6.8.3\msvc2022_64\bin'
        if ($Debugger) {
            # attach (the app runs exactly as without a debugger until here: no debug heap)
            $cdbLog = Join-Path $OutDir "$Tag-run$i.cdb.txt"
            $cdbArgs = @('-lines', '-G', '-y', "`"$sym`"", '-logo', "`"$cdbLog`"", '-cf', "`"$(Join-Path $PSScriptRoot 'cdb-on-av.txt')`"", '-p', "$($app.Id)")
            $host_ = Start-Process -FilePath $cdb -ArgumentList $cdbArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $OutDir "$Tag-run$i.cdb.stdout.txt")
            $null = $host_.Handle
            Start-Sleep -Seconds 5
        }
        # WM_CLOSE (0x0010) to the main window, as Process.CloseMainWindow() does
        $sent = if ($hwnd -ne [IntPtr]::Zero) { [W2067.User32]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) } else { $false }
        $closeAt = Get-Date
        $closes.Add([DateTimeOffset]::new($closeAt).ToUnixTimeSeconds())
        $exited = $app.WaitForExit(60000)
        if (-not $exited) {
            # hang: non-invasive snapshot of every thread's stack, then end the (own) process
            $hangLog = Join-Path $OutDir "$Tag-run$i.hang-stacks.txt"
            $null = Start-Process -FilePath $cdb -ArgumentList @('-pv', '-p', "$($app.Id)", '-y', "`"$sym`"", '-lines', '-logo', "`"$hangLog`"", '-c', '"~* kn 60; qd"') -PassThru -WindowStyle Hidden -Wait
            Note "run $i : app did not exit 60 s after WM_CLOSE - stacks in $(Split-Path -Leaf $hangLog); killed (own process)"
            Stop-Process -Id $app.Id -Force; $null = $app.WaitForExit(10000)
        }
        $app.WaitForExit()
        $code = $app.ExitCode
        $secs = [math]::Round(((Get-Date) - $closeAt).TotalSeconds, 1)
        if ($host_) { $null = $host_.WaitForExit(60000); if (-not $host_.HasExited) { Stop-Process -Id $host_.Id -Force } }
        $tail = @(Get-Content -LiteralPath $errLog -ErrorAction SilentlyContinue | Select-Object -Last 6)
        $ok = $exited -and $code -eq 0
        if (-not $ok) { $failed++ }
        $done++
        Note ("run {0} : WM_CLOSE sent {1}, exited {2} after {3} s, exit code {4} (0x{5:X8}) -> {6}" -f $i, $sent, $exited, $secs, $code, $code, $(if ($ok) { 'PASS' } else { 'FAIL' }))
        foreach ($l in $tail) { Note "    stderr| $l" }
        Start-Sleep -Seconds 2
    }
    # Database after the runs (simulator runs write one sensor_data row per poll cycle): SQLite
    # integrity_check and the newest row before every WM_CLOSE (build\w2-067-dbcheck, docs\evidence\w2-067\tools\build-dbcheck.bat).
    $dbcheck = Join-Path $root 'build\w2-067-dbcheck\w2067_dbcheck.exe'
    if (-not $NoSimulator -and $closes.Count) {
        $dbFile = Join-Path $dataDir ('data\sensor_' + (Get-Date -Format 'yyyyMM') + '.sqlite')
        if (Test-Path -LiteralPath $dbcheck) {
            $dbOut = & $dbcheck $dbFile @($closes | ForEach-Object { "$_" }) 2>&1
            $dbCode = $LASTEXITCODE
            foreach ($l in $dbOut) { Note "    db| $l" }
            Note "database check exit code $dbCode"
            if ($dbCode -ne 0) { $failed++ }
        } else {
            Note "database check skipped: $dbcheck not built"
            $failed++
        }
    }
} finally {
    if ($simPid) {
        $s = Get-Process -Id $simPid -ErrorAction SilentlyContinue
        if ($s -and $s.ProcessName -eq 'Adam60xxSimulator') { $null = $s.CloseMainWindow(); if (-not $s.WaitForExit(10000)) { Stop-Process -Id $simPid -Force }; Note "simulator pid $simPid closed" }
    }
    $left = @(Get-Process | Where-Object { $_.ProcessName -in 'TaidaFlowApp', 'Adam60xxSimulator', 'cdb' })
    Note ("left running afterwards: " + $(if ($left.Count) { ($left | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ' } else { 'none' }))
    Note ("=== result: {0} run(s), {1} failed" -f $done, $failed)
    [System.IO.File]::AppendAllLines($summary, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}
if ($failed -or $done -lt $Runs) { exit 1 }
exit 0

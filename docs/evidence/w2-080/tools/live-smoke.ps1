# w2-080 D5: live smoke of the desktop app with the alarm views (no UI test).
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-080\tools\live-smoke.ps1
#            [-OutDir docs\evidence\w2-080\live] [-UptimeSec 100]
# Needs: build\desktop\TaidaFlowApp.exe, build\w2-080-mirror-alarm-client\mirror_alarm_client.exe
#        (docs\evidence\w2-080\tools\build-mirror-alarm-client.bat), ..\Adam60xxSimulator\build\Adam60xxSimulator.exe.
#
# 1. Refuses (exit 2) while any TaidaFlowApp runs (someone else's instance is never closed).
# 2. Adam60xxSimulator: used when already running; otherwise started with scripts\run-simulator.ps1 and
#    closed again at the end (only the one this script started).
# 3. scripts\safety_probe.ps1 -DeviceProfile simulator with docs\evidence\w2-080\config.w2-080-sim.json
#    (= deploy\dev\config.simulator.json with ports 8224 / 8225 / 18225 / 18180, nginx off and dataDir
#    build\w2-080-run, so that a main-branch TaidaFlowApp on 8125 / 18125 is not in the way) must be SAFE.
# 4. scripts\run-desktop.ps1 -DeviceProfile simulator with that config (it probes again).
# 5. REST GET / and /api/sensor/last (twice, the newest row must move on = data sync), runtime.json on 8224,
#    then the mirror client sends alarmViewRequested (default range, whole range, page past the end,
#    from > to, default range) and prints alarmViews[web-w2080live].
# 6. After UptimeSec the window is closed like a user (WM_CLOSE); the log must end with
#    "[Core] shutdown complete" and no listener of the app may be left.
# Exit 0 = all checks passed; 1 = a check failed; 2 = refused; 3 = safety probe not SAFE.
param(
    [string]$OutDir = "docs\evidence\w2-080\live",
    [int]$UptimeSec = 100
)
$ErrorActionPreference = 'Continue'   # native tools write to stderr (PS 5.1 would turn it into errors)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$summary = Join-Path $OutDir 'summary.txt'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Write-Output $line; Add-Content -Encoding utf8 $summary $line }
Set-Content -Encoding utf8 $summary "=== w2-080 live smoke $(Get-Date -Format o)"
$config = 'docs\evidence\w2-080\config.w2-080-sim.json'
$ports = '(502|8224|8225|18180|18225)'
$failed = 0
function Check([bool]$ok, [string]$what) { if ($ok) { Say "PASS $what" } else { Say "FAIL $what"; $script:failed++ } }

$others = @(Get-Process TaidaFlowApp -ErrorAction SilentlyContinue)
if ($others.Count) { Say ("TaidaFlowApp already running: " + (($others | ForEach-Object { "$($_.Id) $($_.Path)" }) -join '; ') + " - refused, not closed"); exit 2 }

$startedSim = $null
$sim = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($sim.Count) { Say "Adam60xxSimulator already running (pid $($sim[0].Id)) - used, not stopped" }
else {
    $p = Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'scripts\run-simulator.ps1',
            '-LogFile', 'build\w2-080-run-logs\simulator.log') -RedirectStandardOutput (Join-Path $OutDir 'run-simulator.txt') `
            -RedirectStandardError (Join-Path $OutDir 'run-simulator.err.txt') -NoNewWindow -PassThru
    $null = $p.Handle   # exit code readable after the exit
    $p.WaitForExit()
    $text = Get-Content (Join-Path $OutDir 'run-simulator.txt') -Raw
    Say "run-simulator exit=$($p.ExitCode): $($text.Trim())"
    $m = [regex]::Match($text, 'SIM_PID=(\d+)')
    if ($m.Success) { $startedSim = [int]$m.Groups[1].Value } else { Say 'simulator not started'; exit 1 }
}

& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason 'w2-080 live smoke (explicit)' `
    -DeviceProfile simulator -Config $config -LogFile (Join-Path $OutDir 'safety-probe-sim.log') *> (Join-Path $OutDir 'safety-probe.console.txt')
$probe = $LASTEXITCODE
Say "safety_probe exit=$probe ($(if ($probe -eq 0) { 'SAFE' } else { 'NOT SAFE' }))"
if ($probe -ne 0) {
    if ($startedSim) { Stop-Process -Id $startedSim -ErrorAction SilentlyContinue }
    exit 3
}

$p = Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'scripts\run-desktop.ps1',
        '-Label', 'w2-080-live-smoke', '-DeviceProfile', 'simulator', '-Config', $config,
        '-LogFile', 'build\w2-080-run-logs\desktop.log', '-ProbeLog', (Join-Path $OutDir 'safety-probe-sim.log')) `
        -RedirectStandardOutput (Join-Path $OutDir 'run-desktop.txt') -RedirectStandardError (Join-Path $OutDir 'run-desktop.err.txt') -NoNewWindow -PassThru
$null = $p.Handle
$p.WaitForExit()
$text = Get-Content (Join-Path $OutDir 'run-desktop.txt') -Raw
$m = [regex]::Match($text, '(?m)^PID=(\d+)')   # the app's PID line (not a PID inside another word)
if ($p.ExitCode -ne 0 -or -not $m.Success) {
    Say "run-desktop exit=$($p.ExitCode) - app not started"; if ($startedSim) { Stop-Process -Id $startedSim -ErrorAction SilentlyContinue }; exit 1
}
$appPid = [int]$m.Groups[1].Value
$app = Get-Process -Id $appPid
$null = $app.Handle      # keeps the exit code readable
Say "app started pid=$appPid"
Start-Sleep -Seconds 15
$listen = @(netstat -ano | Select-String 'LISTENING' | Select-String " $appPid$" | ForEach-Object { $_.Line.Trim() })
Say ("app listeners: " + ($listen -join ' | '))
Check ($listen.Count -ge 5) 'app listens on its 5 service ports (502, 8224, 8225, 18180, 18225)'

$rest = curl.exe -s -w '|%{http_code}' http://127.0.0.1:18180/
Check ($rest -match '"status":"ok".*\|200$') "REST GET / -> $rest"
$last1 = curl.exe -s http://127.0.0.1:18180/api/sensor/last
$rt = curl.exe -s -w '|%{http_code}' http://127.0.0.1:8224/runtime.json
Check ($rt -match '"mirrorPublicPort":8225.*\|200$') "GET runtime.json -> $rt"

$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
$wideTo = [DateTimeOffset]::Now.ToUnixTimeMilliseconds() + 3600000
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8     # the client prints UTF-8
$clientOut = & build\w2-080-mirror-alarm-client\mirror_alarm_client.exe --url ws://127.0.0.1:8225/mirror --origin http://127.0.0.1:8224 `
    --session web-w2080live --req default:1 --req "0:$($wideTo):1" --req "0:$($wideTo):99" --req "2000:1000:1" --req default:1 `
    --step-sec 5 --linger-sec 10 2>&1
$client = $LASTEXITCODE
[System.IO.File]::WriteAllLines((Join-Path (Resolve-Path $OutDir) 'mirror-alarm-client.txt'), [string[]]($clientOut | ForEach-Object { "$_" }),
                                (New-Object System.Text.UTF8Encoding($false)))
$ctext = Get-Content -Encoding utf8 (Join-Path $OutDir 'mirror-alarm-client.txt') -Raw
Check ($client -eq 0) "mirror client exit=$client"
Check ($ctext -match 'ENTRY rev=\d+ state=ready page=1/1 total=[1-9]') 'alarmViews entry (ready, rows) read back through the mirror'
Check ($ctext -match 'no change of the entry within') 'page past the end -> clamped, identical entry keeps its revision'
Check ($ctext -match 'ENTRY rev=\d+ state=error page=1/1 total=0 active=0 rows=0 range=2000\.\.1000 message=\S') 'from > to -> state error with a message'

while (((Get-Date) - $app.StartTime).TotalSeconds -lt $UptimeSec) { Start-Sleep -Seconds 2 }
$last2 = curl.exe -s http://127.0.0.1:18180/api/sensor/last
$m1 = [regex]::Match("$last1", '"ts":(\d+)')
$m2 = [regex]::Match("$last2", '"ts":(\d+)')
$ts1 = if ($m1.Success) { [int64]$m1.Groups[1].Value } else { 0 }
$ts2 = if ($m2.Success) { [int64]$m2.Groups[1].Value } else { 0 }
Check ($ts2 -gt $ts1 -and $ts1 -gt 0) "sensor data saved while running: /api/sensor/last ts $ts1 -> $ts2"
$uptime = [int]((Get-Date) - $app.StartTime).TotalSeconds
$sent = $app.CloseMainWindow()
$exited = $app.WaitForExit(30000)
Say "uptime ${uptime} s; WM_CLOSE sent=$sent exited=$exited exit_code=$(if ($exited) { $app.ExitCode } else { 'n/a' })"
Check ($exited -and $app.ExitCode -eq 0) 'app closed with exit code 0'
Start-Sleep -Seconds 1
$left = @(netstat -ano | Select-String 'LISTENING' | Select-String " $appPid$")
Check ($left.Count -eq 0) 'no listener of the app left'

$enc = [System.Text.Encoding]::GetEncoding(950)
$log = [System.IO.File]::ReadAllText((Join-Path $root 'build\w2-080-run-logs\desktop.log'), $enc)
[System.IO.File]::WriteAllText((Join-Path (Resolve-Path $OutDir) 'desktop.log'), $log, (New-Object System.Text.UTF8Encoding($false)))
Check ($log -match '\[Core\] shutdown complete') 'log: [Core] shutdown complete'
Check ($log -match '\[Alarm\] per-client views') 'log: alarm view service started'
Check ($log -match '\[Alarm\] web-w2080live #\d+ applied') 'log: alarm view request applied'
$warn = @(($log -split "`r?`n") | Where-Object { $_ -match '(?i)warning|error|fail|critical|fatal' })
Say ("warning/error lines in the log: " + $warn.Count)
$warn | ForEach-Object { Add-Content -Encoding utf8 $summary "    $_" }

if ($startedSim) {
    $s = Get-Process -Id $startedSim -ErrorAction SilentlyContinue
    if ($s) { $null = $s.CloseMainWindow(); if (-not $s.WaitForExit(10000)) { Stop-Process -Id $startedSim -Force } }
    Say "simulator pid $startedSim (started here) closed"
}
Say "=== $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })"
exit $(if ($failed) { 1 } else { 0 })

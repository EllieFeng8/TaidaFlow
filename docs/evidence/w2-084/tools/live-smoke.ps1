# w2-084 D5: live smoke of the server heartbeat without a browser (no UI test: no screenshot, no clicks;
# only the process, its ports, REST, a Mirror test client and the logs).
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-084\tools\live-smoke.ps1
#            [-OutDir docs\evidence\w2-084\live] [-UptimeSec 80]
# Needs: build\desktop\TaidaFlowApp.exe, build\w2-084-mirror-heartbeat-client\mirror_heartbeat_client.exe
#        (docs\evidence\w2-084\tools\build-mirror-heartbeat-client.bat), ..\Adam60xxSimulator\build\Adam60xxSimulator.exe.
#
# 1. Other TaidaFlowApp processes (e.g. the main-branch UI build the PM runs for Mango on 127.0.0.1:8125/18125)
#    are listed and NEVER touched. Only the app started here is closed. Its ports come from
#    docs\evidence\w2-084\config.w2-084-sim.json = the w2-082 test config with http 8244, rest 18195,
#    mirror 18245 / 8245, nginx off and dataDir build\w2-084-run; all must be free (else exit 2).
# 2. Adam60xxSimulator: used when already running; otherwise started with scripts\run-simulator.ps1 and
#    closed again at the end (only the one this script started).
# 3. scripts\safety_probe.ps1 -DeviceProfile simulator with that config must be SAFE (else exit 3).
# 4. The app is started like scripts\run-desktop.ps1 -DeviceProfile simulator (TAIDAFLOW_CONFIG,
#    TAIDAFLOW_DEVICE_PROFILE=simulator, QT_FORCE_STDERR_LOGGING=1, Qt bin on PATH, working folder = dataDir,
#    stderr -> log). run-desktop.ps1 itself refuses while ANY TaidaFlowApp runs (someone else's included).
# 5. The Mirror test client connects to ws://127.0.0.1:8245/mirror (LanRelay, the web page's path when nginx
#    is off) and prints every serverHeartbeatMs change. After UptimeSec the app window is closed like a user
#    (WM_CLOSE); the client must then see the connection close.
# Checks: client exit 0 (>= 50 heartbeats, every step 900..1200 ms, no receive gap > 2.5 s, disconnect seen),
# heartbeat values = the machine's epoch ms, no heartbeat received after the Core stopped it, app log
# "[Heartbeat] server heartbeat started" / "stopped" (stopped before "[Core] shutdown (aboutToQuit)"),
# "[Core] shutdown complete", exit code 0, no QML error, no listener left, other TaidaFlowApp untouched.
# Exit 0 = all checks passed; 1 = a check failed; 2 = refused (port in use / not built); 3 = probe not SAFE.
param(
    [string]$OutDir = "docs\evidence\w2-084\live",
    [int]$UptimeSec = 80
)
$ErrorActionPreference = 'Continue'   # native tools write to stderr (PS 5.1 would turn it into errors)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'summary.txt'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss.fff') $m"; Write-Output $line; Add-Content -Encoding utf8 $summary $line }
Set-Content -Encoding utf8 $summary "=== w2-084 live smoke $(Get-Date -Format o)"
$config = Join-Path $root 'docs\evidence\w2-084\config.w2-084-sim.json'
$exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$client = Join-Path $root 'build\w2-084-mirror-heartbeat-client\mirror_heartbeat_client.exe'
$dataDir = Join-Path $root 'build\w2-084-run'
$logDir = Join-Path $root 'build\w2-084-run-logs'
$appLog = Join-Path $logDir 'desktop.log'
$clientOut = Join-Path $OutDir 'mirror-heartbeat-client.txt'
$ports = @(502, 8244, 8245, 18195, 18245)
$failed = 0
function Check([bool]$ok, [string]$what) { if ($ok) { Say "PASS $what" } else { Say "FAIL $what"; $script:failed++ } }

foreach ($f in @($exe, $client)) { if (-not (Test-Path $f)) { Say "not built: $f"; exit 2 } }
$others = @(Get-Process TaidaFlowApp -ErrorAction SilentlyContinue)
Say ("other TaidaFlowApp processes (not touched): " + $(if ($others.Count) { ($others | ForEach-Object { "$($_.Id) $($_.Path)" }) -join '; ' } else { 'none' }))
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
if ($busy.Count) { Say ("ports in use: " + (($busy | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" }) -join '; ') + " - refused"); exit 2 }
Say "ports free: $($ports -join ', ')"
New-Item -ItemType Directory -Force $dataDir, $logDir | Out-Null

$startedSim = $null
$sim = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($sim.Count) { Say "Adam60xxSimulator already running (pid $($sim[0].Id)) - used, not stopped" }
else {
    $p = Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'scripts\run-simulator.ps1',
            '-LogFile', 'build\w2-084-run-logs\simulator.log') -RedirectStandardOutput (Join-Path $OutDir 'run-simulator.txt') `
            -RedirectStandardError (Join-Path $OutDir 'run-simulator.err.txt') -NoNewWindow -PassThru
    $null = $p.Handle
    $p.WaitForExit()
    $text = Get-Content (Join-Path $OutDir 'run-simulator.txt') -Raw
    Say "run-simulator exit=$($p.ExitCode): $("$text".Trim())"
    $m = [regex]::Match("$text", 'SIM_PID=(\d+)')
    if ($m.Success) { $startedSim = [int]$m.Groups[1].Value } else { Say 'simulator not started'; exit 1 }
}
function Stop-OwnSimulator {
    if ($script:startedSim) {
        $s = Get-Process -Id $script:startedSim -ErrorAction SilentlyContinue
        if ($s) { $null = $s.CloseMainWindow(); if (-not $s.WaitForExit(10000)) { Stop-Process -Id $script:startedSim -Force } }
        Say "simulator pid $($script:startedSim) (started here) closed"
    }
}

& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason 'w2-084 live smoke (explicit)' `
    -DeviceProfile simulator -Config $config -Exe $exe -LogFile (Join-Path $OutDir 'safety-probe-sim.log') *> (Join-Path $OutDir 'safety-probe.console.txt')
$probe = $LASTEXITCODE
Say "safety_probe exit=$probe ($(if ($probe -eq 0) { 'SAFE' } else { 'NOT SAFE' }))"
if ($probe -ne 0) { Stop-OwnSimulator; exit 3 }

# Same launch as scripts\run-desktop.ps1 -DeviceProfile simulator (see 4. above).
$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_CONFIG = $config
$env:TAIDAFLOW_DEVICE_PROFILE = 'simulator'
$appFullLogBefore = @(Get-ChildItem (Join-Path $dataDir 'logs') -Filter '*-full.log' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
$app = Start-Process -FilePath $exe -WorkingDirectory $dataDir -PassThru -RedirectStandardError $appLog -RedirectStandardOutput "$appLog.stdout"
$null = $app.Handle      # keeps the exit code readable
$appPid = $app.Id
Say "app started pid=$appPid (config $config, dataDir $dataDir)"
Start-Sleep -Seconds 12
$listen = @(netstat -ano | Select-String 'LISTENING' | Select-String " $appPid$" | ForEach-Object { $_.Line.Trim() })
Say ("app listeners: " + ($listen -join ' | '))
Check ($listen.Count -ge 5) 'app listens on its 5 service ports (502, 8244, 8245, 18195, 18245)'
$rest = curl.exe -s -w '|%{http_code}' http://127.0.0.1:18195/
Check ($rest -match '"status":"ok".*\|200$') "REST GET / -> $rest"

# Mirror test client in the background (its own process; stdout -> file).
$cl = Start-Process -FilePath $client -ArgumentList @('--url', 'ws://127.0.0.1:8245/mirror', '--origin', 'http://127.0.0.1:8244',
        '--duration-sec', '240', '--min-beats', '50', '--expect-disconnect') -PassThru -NoNewWindow `
        -RedirectStandardOutput $clientOut -RedirectStandardError (Join-Path $OutDir 'mirror-heartbeat-client.err.txt')
$null = $cl.Handle
Say "mirror client started pid=$($cl.Id) -> ws://127.0.0.1:8245/mirror"

while (((Get-Date) - $app.StartTime).TotalSeconds -lt $UptimeSec) { Start-Sleep -Seconds 2 }
$uptime = [int]((Get-Date) - $app.StartTime).TotalSeconds
Check ($uptime -ge 60) "uptime $uptime s >= 60 s"
Check (-not $cl.HasExited) 'mirror client still connected before the app is closed'
$closeAt = Get-Date
$sent = $app.CloseMainWindow()
$exited = $app.WaitForExit(30000)
Say "WM_CLOSE sent=$sent at $($closeAt.ToString('HH:mm:ss.fff')) exited=$exited exit_code=$(if ($exited) { $app.ExitCode } else { 'n/a' })"
Check ($exited -and $app.ExitCode -eq 0) 'app closed with exit code 0'
$clientDone = $cl.WaitForExit(30000)
Say "mirror client exited=$clientDone exit_code=$(if ($clientDone) { $cl.ExitCode } else { 'n/a' })"
if (-not $clientDone) { Stop-Process -Id $cl.Id -Force -ErrorAction SilentlyContinue }
Check ($clientDone -and $cl.ExitCode -eq 0) 'mirror client exit 0 (>= 50 heartbeats, steps 900..1200 ms, gaps <= 2.5 s, disconnect seen)'
Start-Sleep -Seconds 1
$left = @(netstat -ano | Select-String 'LISTENING' | Select-String " $appPid$")
Check ($left.Count -eq 0) 'no listener of the app left'
$stillOthers = @($others | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
Check ($stillOthers.Count -eq $others.Count) "other TaidaFlowApp processes still running: $($stillOthers.Count) of $($others.Count)"

# ---- client output ----
$clientLines = @(Get-Content $clientOut -Encoding utf8)
$beats = @($clientLines | Where-Object { $_ -match ' BEAT #' })
$result = ($clientLines | Where-Object { $_ -match 'RESULT ' } | Select-Object -Last 1)
$disc = ($clientLines | Where-Object { $_ -cmatch 'DISCONNECTED after' } | Select-Object -Last 1)
$hash = ($clientLines | Where-Object { $_ -match 'contractHash=' } | Select-Object -First 1)
Say "client: $hash"
Say "client: $($beats.Count) BEAT lines; first: $($beats | Select-Object -First 1)"
Say "client: last: $($beats | Select-Object -Last 1)"
Say "client: $disc"
Say "client: $result"
Check (@($clientLines -match 'snapshot: serverHeartbeatMs=[1-9]').Count -eq 1) 'snapshot already carries a heartbeat (Core wrote it at start-up)'
Check ([bool]$disc) "client saw the connection close after WM_CLOSE: $disc"
# Heartbeat value vs the client's receive time (same machine, same clock): within 1 s.
$skews = @()
foreach ($b in $beats) {
    $m = [regex]::Match($b, '^(\d\d):(\d\d):(\d\d)\.(\d\d\d) BEAT #\d+ value=(\d+) ')
    if (-not $m.Success) { continue }
    $recv = $closeAt.Date.AddHours([int]$m.Groups[1].Value).AddMinutes([int]$m.Groups[2].Value).AddSeconds([int]$m.Groups[3].Value).AddMilliseconds([int]$m.Groups[4].Value)
    $valueTime = [DateTimeOffset]::FromUnixTimeMilliseconds([int64]$m.Groups[5].Value).LocalDateTime
    $skews += [math]::Abs(($recv - $valueTime).TotalMilliseconds)
}
$maxSkew = if ($skews.Count) { [int]($skews | Measure-Object -Maximum).Maximum } else { -1 }
Check ($skews.Count -eq $beats.Count -and $maxSkew -ge 0 -and $maxSkew -lt 1000) "heartbeat value = epoch ms of the Core (max |receive time - value| = $maxSkew ms over $($skews.Count) beats)"

# ---- app logs ----
$enc = [System.Text.Encoding]::GetEncoding(950)
$log = [System.IO.File]::ReadAllText($appLog, $enc)
[System.IO.File]::WriteAllText((Join-Path $OutDir 'desktop.log'), $log, (New-Object System.Text.UTF8Encoding($false)))
$full = @(Get-ChildItem (Join-Path $dataDir 'logs') -Filter '*-full.log' | Sort-Object LastWriteTime | Select-Object -Last 1)
$fullText = if ($full.Count) { [System.IO.File]::ReadAllText($full[0].FullName, [System.Text.Encoding]::UTF8) } else { '' }
# This run's part of the full log (it is appended per day): from the last "[Heartbeat] ... started" backwards to the
# preceding "[Config] config file" line.
$fullLines = $fullText -split "`r?`n"
$startIdx = -1
for ($i = $fullLines.Count - 1; $i -ge 0; $i--) { if ($fullLines[$i] -match '\[Config\] config file ') { $startIdx = $i; break } }
$runLines = if ($startIdx -ge 0) { $fullLines[$startIdx..($fullLines.Count - 1)] } else { @() }
[System.IO.File]::WriteAllLines((Join-Path $OutDir 'app-full-log-this-run.txt'), [string[]]$runLines, (New-Object System.Text.UTF8Encoding($false)))
$hbLines = @($runLines | Where-Object { $_ -match '\[Heartbeat\]|\[Core\] shutdown' })
$hbLines | ForEach-Object { Say "  app log: $_" }
Check ($log -match '\[Heartbeat\] server heartbeat started: serverHeartbeatMs = epoch ms every 1000 ms \(main thread\)') 'log: [Heartbeat] server heartbeat started ... every 1000 ms (main thread)'
$stopM = [regex]::Match($log, '\[Heartbeat\] server heartbeat stopped after (\d+) write\(s\)')
Check ($stopM.Success) "log: [Heartbeat] server heartbeat stopped after $($stopM.Groups[1].Value) write(s)"
if ($stopM.Success) {
    $writes = [int]$stopM.Groups[1].Value
    Check ([math]::Abs($writes - $uptime) -le 15) "heartbeat writes $writes ~ one per second of uptime ($uptime s, init takes a few seconds)"
}
$iStop = $log.IndexOf('[Heartbeat] server heartbeat stopped')
$iShut = $log.IndexOf('[Core] shutdown (aboutToQuit): stopping the backend')
Check ($iStop -ge 0 -and $iShut -gt $iStop) 'log: heartbeat stopped BEFORE "[Core] shutdown (aboutToQuit): stopping the backend"'
Check ($log -match '\[Core\] shutdown complete') 'log: [Core] shutdown complete'
# Timing (full log has timestamps): no heartbeat reached the client after the Core stopped it.
$stopLine = @($runLines | Where-Object { $_ -match '\[Heartbeat\] server heartbeat stopped' }) | Select-Object -First 1
$sm = [regex]::Match("$stopLine", '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d\d\d)')
$lastBeat = $beats | Select-Object -Last 1
$bm = [regex]::Match("$lastBeat", '^(\d\d:\d\d:\d\d\.\d\d\d) BEAT #\d+ value=(\d+)')
if ($sm.Success -and $bm.Success) {
    $stopAt = [datetime]::ParseExact($sm.Groups[1].Value, 'yyyy-MM-dd HH:mm:ss.fff', $null)
    $lastValue = [DateTimeOffset]::FromUnixTimeMilliseconds([int64]$bm.Groups[2].Value).LocalDateTime
    Say ("  heartbeat stopped at {0:HH:mm:ss.fff}; last heartbeat value received {1:HH:mm:ss.fff}" -f $stopAt, $lastValue)
    Check ($lastValue -le $stopAt) 'the last heartbeat the client received was written before the Core stopped the heartbeat'
    Check (($stopAt - $lastValue).TotalMilliseconds -le 1100) 'the last heartbeat is the one written just before the stop (<= 1.1 s)'
} else { Check $false 'timestamps of the heartbeat stop / last beat found' }
$qmlErr = @(($log -split "`r?`n") | Where-Object { $_ -match '(?i)qrc:/.*(error|warning)|TypeError|ReferenceError|is not a type|module .* is not installed' })
Check ($qmlErr.Count -eq 0) "log: no QML error/warning lines ($($qmlErr.Count))"
$warn = @(($log -split "`r?`n") | Where-Object { $_ -match '(?i)warning|error|fail|critical|fatal' })
Say ("warning/error lines in the log: " + $warn.Count)
$warn | ForEach-Object { Add-Content -Encoding utf8 $summary "    $_" }
$hbWarn = @($warn | Where-Object { $_ -match 'Heartbeat' })
Check ($hbWarn.Count -eq 0) 'log: no heartbeat warning'

Stop-OwnSimulator
Say "=== $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })"
exit $(if ($failed) { 1 } else { 0 })

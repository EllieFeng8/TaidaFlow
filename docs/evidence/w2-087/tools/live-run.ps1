# w2-087 D5: live run of the device offline status (TaidaFlowProxy::deviceStatus) with the task's own
# desktop build, without a browser and without UI test (no screenshot, no clicks): the process, its
# ports, a Proxy Mirror test client that reads deviceStatus like the web page, and the app log.
# Adapted from docs\evidence\w2-086\tools\live-run.ps1 (gates as in w2-072).
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-087\tools\live-run.ps1
#            [-OutDir docs\evidence\w2-087\live] [-UptimeSec 75] [-ClientSec 65]
# Needs: build\w2-087-desktop\TaidaFlowApp.exe, build\w2-087-wasm (web folder), build\w2-087-run\tools
#        (tools\build-live-tools.bat) and a RUNNING Adam60xxSimulator (shared, never started or stopped here).
#
# * Two configs of the task (all listeners 127.0.0.1: Modbus server 5397, http 8397, REST 18397, mirror
#   18398 / 8398, nginx off):
#     config.w2-087-sim.json     - the five ADAM at the simulator 127.0.0.201..205:502, MS300 COM2
#     config.w2-087-offline.json - the same, but ADAM-6217 B at 127.0.0.210:502 (loopback, nobody listens)
#   Gate 1: safety_probe -DeviceProfile simulator with config.w2-087-sim.json must be SAFE (exit 0).
#   Gate 2: safety_probe with config.w2-087-offline.json must be exit 5 (SIMULATOR-NOT-READY) with exactly
#           one not-ready endpoint, 127.0.0.210:502 of devices.adam6217b, and nothing UNSAFE / BUSY.
#   Only then the app starts with config.w2-087-offline.json, WITHOUT TAIDAFLOW_DEVICE_PROFILE (the
#   configured hosts are used), data folder build\w2-087-run\live (deleted at the start).
# * Other TaidaFlowApp / nginx / simulator processes are listed and never touched; only the app started
#   here is closed (WM_CLOSE).
# * Client (mirror_device_status_client, ClientSec s): the final deviceStatus must be exactly
#   adam6256/6217a/6224/6022 online true with name = model and address = 127.0.0.20x, adam6217b
#   ADAM-6217 127.0.0.210 online false, ms300 MS300 COM2 online false.
# * Log checks (full log): one first-result line per device, ADAM-6217 B OFFLINE once although its
#   reconnect attempts keep failing, MS300 OFFLINE once although its open attempts keep failing,
#   7 writes until the shutdown, the shutdown writes {} between "[Core] shutdown (aboutToQuit)" and
#   "[Core] shutdown complete", no QML error.
# Exit 0 = all checks passed; 1 = a check failed; 2 = refused (not built / port in use / no simulator);
# 3 = a safety gate did not give the expected verdict.
param(
    [string]$OutDir = "docs\evidence\w2-087\live",
    [int]$UptimeSec = 75,
    [int]$ClientSec = 65
)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'summary.txt'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss.fff') $m"; Write-Host $line; Add-Content -Encoding utf8 $summary $line }
Set-Content -Encoding utf8 $summary "=== w2-087 live run $(Get-Date -Format o)"
$simConfig = Join-Path $root 'docs\evidence\w2-087\config.w2-087-sim.json'
$config = Join-Path $root 'docs\evidence\w2-087\config.w2-087-offline.json'
$exe = Join-Path $root 'build\w2-087-desktop\TaidaFlowApp.exe'
$webDir = Join-Path $root 'build\w2-087-wasm'
$client = Join-Path $root 'build\w2-087-run\tools\mirror_device_status_client.exe'
$dataDir = Join-Path $root 'build\w2-087-run\live'
$ports = @(5397, 8397, 8398, 18397, 18398)
$failed = 0
function Check([bool]$ok, [string]$what) { if ($ok) { Say "PASS $what" } else { Say "FAIL $what"; $script:failed++ } }

foreach ($f in @($exe, $client, $simConfig, $config, (Join-Path $webDir 'TaidaFlowApp.html'))) { if (-not (Test-Path $f)) { Say "not built / missing: $f"; exit 2 } }
$others = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue)
Say ("other TaidaFlowApp / nginx processes (not touched): " + $(if ($others.Count) { ($others | ForEach-Object { "$($_.ProcessName) $($_.Id) $($_.Path)" }) -join '; ' } else { 'none' }))
$sim = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if (-not $sim.Count) { Say 'Adam60xxSimulator is not running - refused (start it first; it is shared, not started here)'; exit 2 }
Say "Adam60xxSimulator running (pid $($sim[0].Id)) - shared, not stopped"
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
if ($busy.Count) { Say ("ports in use: " + (($busy | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" }) -join '; ') + " - refused"); exit 2 }
Say "ports free: $($ports -join ', ')"

# ---- the test config: every device host loopback, only adam6217b changed ----
$cfgSim = Get-Content $simConfig -Raw -Encoding utf8 | ConvertFrom-Json
$cfgOff = Get-Content $config -Raw -Encoding utf8 | ConvertFrom-Json
$hosts = @('adam6256', 'adam6217a', 'adam6217b', 'adam6224', 'adam6022') | ForEach-Object { $cfgOff.devices.$_.host }
Say "test config $config device hosts: $($hosts -join ', '); ms300 $($cfgOff.devices.ms300.serialPort)"
Check (@($hosts | Where-Object { $_ -notlike '127.*' }).Count -eq 0) "test config: every device host is 127.x"
Check ($cfgOff.devices.adam6217b.host -eq '127.0.0.210' -and $cfgSim.devices.adam6217b.host -eq '127.0.0.203') "test config: devices.adam6217b.host = 127.0.0.210 (sim config 127.0.0.203)"
$diffKeys = @('adam6256', 'adam6217a', 'adam6224', 'adam6022') | Where-Object { $cfgOff.devices.$_.host -ne $cfgSim.devices.$_.host }
Check ($diffKeys.Count -eq 0 -and $cfgOff.modbusServer.port -eq $cfgSim.modbusServer.port -and $cfgOff.modbusServer.port -ne 502 -and -not $cfgOff.nginx.enabled) "test config: other devices and ports as the sim config; Modbus server port $($cfgOff.modbusServer.port) (not 502); nginx off"

# ---- gates ----
& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason "w2-087 live gate 1" `
    -DeviceProfile simulator -Config $simConfig -Exe $exe -LogFile (Join-Path $OutDir 'safety-probe.log') *> (Join-Path $OutDir 'safety-probe-gate1.console.txt')
$rc1 = $LASTEXITCODE
Check ($rc1 -eq 0) "gate 1: safety_probe config.w2-087-sim.json -DeviceProfile simulator -> exit $rc1 (expected 0 SAFE)"
& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason "w2-087 live gate 2 (test config)" `
    -Config $config -Exe $exe -LogFile (Join-Path $OutDir 'safety-probe.log') *> (Join-Path $OutDir 'safety-probe-gate2.console.txt')
$rc2 = $LASTEXITCODE
$g2 = @(Get-Content (Join-Path $OutDir 'safety-probe-gate2.console.txt') -Encoding utf8)
$notReady = @($g2 | Where-Object { $_ -match 'NOT READY' })
$bad = @($g2 | Where-Object { $_ -match 'UNSAFE|BUSY' -and $_ -notmatch 'verdict' })
Check ($rc2 -eq 5 -and $notReady.Count -eq 1 -and $notReady[0] -match '127\.0\.0\.210:502 \[effective devices\.adam6217b' -and $bad.Count -eq 0) "gate 2: safety_probe test config -> exit $rc2 (expected 5); not-ready: $($notReady -join ' | '); UNSAFE/BUSY lines: $($bad.Count)"
if ($rc1 -ne 0 -or $failed) { Say 'gates not passed - the app is NOT started'; exit 3 }

if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
New-Item -ItemType Directory -Force $dataDir | Out-Null
Say "data folder (fresh): $dataDir"
$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_CONFIG = $config
Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue
$env:TAIDAFLOW_WEB_DIR = $webDir

$appLog = Join-Path $OutDir 'app.stderr.log'
$app = Start-Process -FilePath $exe -WorkingDirectory $dataDir -PassThru -RedirectStandardError $appLog -RedirectStandardOutput "$appLog.stdout"
$null = $app.Handle
Say "app started pid=$($app.Id) (TAIDAFLOW_CONFIG=config.w2-087-offline.json, no TAIDAFLOW_DEVICE_PROFILE)"
Start-Sleep -Seconds 3
$listen = @(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue)
Say ("app listeners: " + (($listen | Sort-Object LocalPort | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ' | '))
$own = @($listen | Where-Object { $ports -contains $_.LocalPort })
$notLoopback = @($listen | Where-Object { $_.LocalAddress -ne '127.0.0.1' })
Check ($own.Count -eq 5 -and $notLoopback.Count -eq 0) "app listens on its 5 ports ($($ports -join ', ')), loopback only"

$clientOut = Join-Path $OutDir 'client.txt'
$expect = @('--expect', 'adam6256=ADAM-6256|127.0.0.201|true', '--expect', 'adam6217a=ADAM-6217|127.0.0.202|true',
            '--expect', 'adam6217b=ADAM-6217|127.0.0.210|false', '--expect', 'adam6224=ADAM-6224|127.0.0.204|true',
            '--expect', 'adam6022=ADAM-6022|127.0.0.205|true', '--expect', 'ms300=MS300|COM2|false')
$cl = Start-Process -FilePath $client -ArgumentList (@('--url', 'ws://127.0.0.1:8398/mirror', '--origin', 'http://127.0.0.1:8397',
        '--duration-sec', "$ClientSec", '--max-changes', '6') + $expect) `
        -PassThru -NoNewWindow -RedirectStandardOutput $clientOut -RedirectStandardError "$clientOut.err"
$null = $cl.Handle
$done = $cl.WaitForExit(($ClientSec + 30) * 1000)
if (-not $done) { Stop-Process -Id $cl.Id -Force }
Say "client exit=$(if ($done) { $cl.ExitCode } else { 'timeout' })"
Check ($done -and $cl.ExitCode -eq 0) "mirror client: final deviceStatus as expected (4 ADAM online, ADAM-6217 127.0.0.210 and MS300 COM2 offline, names / addresses)"
Get-Content $clientOut -Encoding utf8 | Where-Object { $_ -match 'DEVICESTATUS|BANNER|PASS|FAIL|RESULT|contractHash|welcome' } | ForEach-Object { Say "  client: $_" }
while (((Get-Date) - $app.StartTime).TotalSeconds -lt $UptimeSec) { Start-Sleep -Seconds 1 }
$uptime = [int]((Get-Date) - $app.StartTime).TotalSeconds
$sent = $app.CloseMainWindow()
$exited = $app.WaitForExit(30000)
Say "uptime $uptime s; WM_CLOSE sent=$sent exited=$exited exit_code=$(if ($exited) { $app.ExitCode } else { 'n/a' })"
Check ($uptime -ge 60) "app ran $uptime s (>= 60 s)"
Check ($exited -and $app.ExitCode -eq 0) "app closed with exit code 0"
Start-Sleep -Seconds 1
$left = @(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue)
Check ($left.Count -eq 0) "no listener of the app left"

# ---- the app's full log ----
$full = @(Get-ChildItem (Join-Path $dataDir 'logs') -Filter '*-full.log' | Sort-Object LastWriteTime)
$fullText = ($full | ForEach-Object { [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8) }) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $OutDir 'app-full-log.txt'), $fullText, (New-Object System.Text.UTF8Encoding($false)))
$lines = $fullText -split "`r?`n"
$ds = @($lines | Where-Object { $_ -match '\[DeviceStatus\]' })
Set-Content -Encoding utf8 (Join-Path $OutDir 'log-devicestatus.txt') $ds
$ds | ForEach-Object { Say "  log: $_" }
$first = @($ds | Where-Object { $_ -match ', first result\)' })
Check ($first.Count -eq 6) "log: one first-result line per device ($($first.Count) of 6)"
$off6217b = @($ds | Where-Object { $_ -match 'ADAM-6217 127\.0\.0\.210 \(adam6217b\): OFFLINE' })
$attempts6217b = @($lines | Where-Object { $_ -match '\[Modbus\] ADAM-6217 B \(127\.0\.0\.210\) reconnect attempt #' })
Check ($off6217b.Count -eq 1 -and $attempts6217b.Count -ge 5) "log: ADAM-6217 B OFFLINE written once ($($off6217b.Count)) while $($attempts6217b.Count) reconnect attempts failed"
$offMs300 = @($ds | Where-Object { $_ -match 'MS300 COM2 \(ms300\): OFFLINE' })
$attemptsMs300 = @($lines | Where-Object { $_ -match '\[MS300\] COM2 connection attempt #' })
Check ($offMs300.Count -eq 1 -and $attemptsMs300.Count -ge 5) "log: MS300 OFFLINE written once ($($offMs300.Count)) while $($attemptsMs300.Count) open attempts failed"
foreach ($d in @('ADAM-6256 127\.0\.0\.201 \(adam6256\)', 'ADAM-6217 127\.0\.0\.202 \(adam6217a\)', 'ADAM-6224 127\.0\.0\.204 \(adam6224\)', 'ADAM-6022 127\.0\.0\.205 \(adam6022\)')) {
    $on = @($ds | Where-Object { $_ -match "$d`: online \(connected, first result\)" })
    $offAny = @($ds | Where-Object { $_ -match "$d`: OFFLINE" })
    Check ($on.Count -eq 1 -and $offAny.Count -eq 0) "log: $($d -replace '\\', '') online once, never OFFLINE"
}
$writes = @($ds | Where-Object { $_ -match '\[DeviceStatus\] write #(\d+): ' })
$beforeStop = @($writes | Where-Object { $_ -notmatch 'write #\d+: \{\}' })
$lastWrite = if ($writes.Count) { $writes[-1] } else { '' }
Check ($beforeStop.Count -eq 6 -and $writes.Count -eq 8 -and $lastWrite -match 'write #8: \{\}') "log: writes = start {} + 6 first results + shutdown {} ($($writes.Count) writes, last: $lastWrite)"
$iShut = [array]::FindIndex($lines, [Predicate[string]]{ param($l) $l -match '\[Core\] shutdown \(aboutToQuit\)' })
$iStop = [array]::FindIndex($lines, [Predicate[string]]{ param($l) $l -match '\[DeviceStatus\] stopped: deviceStatus = \{\}' })
$iDone = [array]::FindIndex($lines, [Predicate[string]]{ param($l) $l -match '\[Core\] shutdown complete' })
Check ($iShut -ge 0 -and $iStop -gt $iShut -and $iDone -gt $iStop) "log: shutdown writes {} inside Core::shutdown (lines $iShut < $iStop < $iDone)"
$afterStop = @($lines[($iStop + 1)..($lines.Count - 1)] | Where-Object { $_ -match '\[DeviceStatus\] (ADAM|MS300)' })
Check ($afterStop.Count -eq 0) "log: no device state written after the stop ($($afterStop.Count))"
$qmlErr = @($lines | Where-Object { $_ -match '(?i)qrc:/.*(error|warning)|TypeError|ReferenceError|is not a type|module .* is not installed' })
Check ($qmlErr.Count -eq 0) "log: no QML error/warning lines ($($qmlErr.Count))"
$stillOthers = @($others | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
Check ($stillOthers.Count -eq $others.Count) "other TaidaFlowApp / nginx processes still running: $($stillOthers.Count) of $($others.Count)"
Check ([bool](Get-Process -Id $sim[0].Id -ErrorAction SilentlyContinue)) "Adam60xxSimulator pid $($sim[0].Id) still running"
Say "=== $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })"
exit $(if ($failed) { 1 } else { 0 })

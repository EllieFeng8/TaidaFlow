# w2-089 D4 live: the device offline status (TaidaFlowProxy::deviceStatus) with the task's own desktop
# build, without a browser and without UI test (no screenshot, no clicks): the process, its ports, a
# Proxy Mirror test client that reads deviceStatus like the web page, and the app log.
# Adapted from docs\evidence\w2-087\tools\live-run.ps1 (that file is not used).
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-089\tools\live-run.ps1
#            [-OutDir docs\evidence\w2-089\live] [-UptimeSec 60] [-ClientSec 50]
# Needs: build\w2-089-desktop\TaidaFlowApp.exe, build\w2-089-wasm (web folder), build\w2-089-run\tools
#        (tools\build-live-tools.bat) and a RUNNING Adam60xxSimulator (shared, never started or stopped here).
#
# * Config docs\evidence\w2-089\config.w2-089-sim.json: the five ADAM at the simulator 127.0.0.201..205:502,
#   MS300 COM2 (no such port on this PC), every listener 127.0.0.1 (Modbus server 5389 - not 502 -, http
#   8389, REST 18389, mirror 18390 / 8390), nginx off, dataDir build\w2-089-run\live (deleted at the start).
# * Gate: scripts\safety_probe.ps1 -DeviceProfile simulator with that config must be SAFE (exit 0);
#   otherwise the app is NOT started. The app then runs with TAIDAFLOW_DEVICE_PROFILE=simulator (the
#   profile the gate checked).
# * Other TaidaFlowApp / nginx / simulator processes are listed and never touched; only the app started
#   here is closed (WM_CLOSE).
# * Client (mirror_device_status_client, ClientSec s): the final deviceStatus must be exactly the five
#   ADAM online true (name = model, address = 127.0.0.20x) and ms300 = name U+5FAA U+74B0 U+6CF5 U+8B8A U+983B U+5668 (the circulation pump inverter, w2-089),
#   address COM2, online false.
# * Log checks (full log): one first-result line per device, the ms300 line with the new name, never a
#   [DeviceStatus] line naming MS300, the [MS300] connection log tag unchanged, 7 writes until the
#   shutdown + the shutdown {} inside Core::shutdown, no QML error.
# The new name is built from its code points (U+5FAA U+74B0 U+6CF5 U+8B8A U+983B U+5668), so this file
# needs no BOM for Windows PowerShell 5.1.
# Exit 0 = all checks passed; 1 = a check failed; 2 = refused (not built / port in use / no simulator);
# 3 = the safety gate was not SAFE.
param(
    [string]$OutDir = "docs\evidence\w2-089\live",
    [int]$UptimeSec = 60,
    [int]$ClientSec = 50
)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'summary.txt'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss.fff') $m"; Write-Host $line; Add-Content -Encoding utf8 $summary $line }
Set-Content -Encoding utf8 $summary "=== w2-089 live run $(Get-Date -Format o) HEAD $(git rev-parse --short HEAD)"
$newName = -join ([char[]](0x5FAA, 0x74B0, 0x6CF5, 0x8B8A, 0x983B, 0x5668))
$config = Join-Path $root 'docs\evidence\w2-089\config.w2-089-sim.json'
$exe = Join-Path $root 'build\w2-089-desktop\TaidaFlowApp.exe'
$webDir = Join-Path $root 'build\w2-089-wasm'
$client = Join-Path $root 'build\w2-089-run\tools\mirror_device_status_client.exe'
$dataDir = Join-Path $root 'build\w2-089-run\live'
$ports = @(5389, 8389, 8390, 18389, 18390)
$failed = 0
function Check([bool]$ok, [string]$what) { if ($ok) { Say "PASS $what" } else { Say "FAIL $what"; $script:failed++ } }

foreach ($f in @($exe, $client, $config, (Join-Path $webDir 'TaidaFlowApp.html'))) { if (-not (Test-Path $f)) { Say "not built / missing: $f"; exit 2 } }
$others = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue)
Say ("other TaidaFlowApp / nginx processes (not touched): " + $(if ($others.Count) { ($others | ForEach-Object { "$($_.ProcessName) $($_.Id) $($_.Path)" }) -join '; ' } else { 'none' }))
$sim = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if (-not $sim.Count) { Say 'Adam60xxSimulator is not running - refused (it is shared, never started here)'; exit 2 }
Say "Adam60xxSimulator running (pid $($sim[0].Id), $($sim[0].Path)) - shared, not stopped"
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
if ($busy.Count) { Say ("ports in use: " + (($busy | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" }) -join '; ') + " - refused"); exit 2 }
Say "ports free: $($ports -join ', ')"

$cfg = Get-Content $config -Raw -Encoding utf8 | ConvertFrom-Json
$keys = @('adam6256', 'adam6217a', 'adam6217b', 'adam6224', 'adam6022')
$hosts = $keys | ForEach-Object { $cfg.devices.$_.host }
Say "config $config device hosts: $($hosts -join ', '); ms300 $($cfg.devices.ms300.serialPort); dataDir $($cfg.dataDir)"
Check (($hosts -join ',') -eq '127.0.0.201,127.0.0.202,127.0.0.203,127.0.0.204,127.0.0.205') "config: the five ADAM at the simulator addresses 127.0.0.201..205"
Check ($cfg.modbusServer.port -eq 5389 -and $cfg.modbusServer.bind -eq '127.0.0.1' -and -not $cfg.nginx.enabled -and $cfg.http.port -eq 8389 -and $cfg.rest.port -eq 18389 -and $cfg.mirror.publicPort -eq 8390 -and $cfg.mirror.internalPort -eq 18390) "config: own ports (Modbus server 127.0.0.1:5389, not 502; http 8389; REST 18389; mirror 18390/8390); nginx off"

# ---- gate ----
& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason "w2-089 live gate" `
    -DeviceProfile simulator -Config $config -Exe $exe -LogFile (Join-Path $OutDir 'safety-probe.log') *> (Join-Path $OutDir 'safety-probe-gate.console.txt')
$rc = $LASTEXITCODE
Check ($rc -eq 0) "gate: safety_probe config.w2-089-sim.json -DeviceProfile simulator -> exit $rc (expected 0 SAFE)"
Get-Content (Join-Path $OutDir 'safety-probe-gate.console.txt') -Encoding utf8 | Where-Object { $_ -match 'verdict|SAFE' } | Select-Object -Last 3 | ForEach-Object { Say "  probe: $_" }
if ($rc -ne 0 -or $failed) { Say 'gate not passed - the app is NOT started'; exit 3 }

if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
New-Item -ItemType Directory -Force $dataDir | Out-Null
Say "data folder (fresh): $dataDir"
$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_CONFIG = $config
$env:TAIDAFLOW_DEVICE_PROFILE = 'simulator'
$env:TAIDAFLOW_WEB_DIR = $webDir

$appLog = Join-Path $OutDir 'app.stderr.log'
$app = Start-Process -FilePath $exe -WorkingDirectory $dataDir -PassThru -RedirectStandardError $appLog -RedirectStandardOutput "$appLog.stdout"
$null = $app.Handle
Say "app started pid=$($app.Id) (TAIDAFLOW_CONFIG=config.w2-089-sim.json, TAIDAFLOW_DEVICE_PROFILE=simulator)"
Start-Sleep -Seconds 3
$listen = @(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue)
Say ("app listeners: " + (($listen | Sort-Object LocalPort | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ' | '))
$own = @($listen | Where-Object { $ports -contains $_.LocalPort })
$notLoopback = @($listen | Where-Object { $_.LocalAddress -ne '127.0.0.1' })
Check ($own.Count -eq 5 -and $notLoopback.Count -eq 0) "app listens on its 5 ports ($($ports -join ', ')), loopback only"

$clientOut = Join-Path $OutDir 'client.txt'
$expect = @('--expect', 'adam6256=ADAM-6256|127.0.0.201|true', '--expect', 'adam6217a=ADAM-6217|127.0.0.202|true',
            '--expect', 'adam6217b=ADAM-6217|127.0.0.203|true', '--expect', 'adam6224=ADAM-6224|127.0.0.204|true',
            '--expect', 'adam6022=ADAM-6022|127.0.0.205|true', '--expect', "ms300=$newName|COM2|false")
$cl = Start-Process -FilePath $client -ArgumentList (@('--url', 'ws://127.0.0.1:8390/mirror', '--origin', 'http://127.0.0.1:8389',
        '--duration-sec', "$ClientSec", '--max-changes', '6') + $expect) `
        -PassThru -NoNewWindow -RedirectStandardOutput $clientOut -RedirectStandardError "$clientOut.err"
$null = $cl.Handle
$done = $cl.WaitForExit(($ClientSec + 30) * 1000)
if (-not $done) { Stop-Process -Id $cl.Id -Force }
Say "client exit=$(if ($done) { $cl.ExitCode } else { 'timeout' })"
Check ($done -and $cl.ExitCode -eq 0) "mirror client: final deviceStatus = 5 ADAM online (names / addresses) + ms300 $newName COM2 offline"
$clientLines = @([System.IO.File]::ReadAllLines($clientOut, [System.Text.Encoding]::UTF8))
$clientLines | Where-Object { $_ -match 'DEVICESTATUS|BANNER|PASS|FAIL|RESULT|contractHash|welcome' } | ForEach-Object { Say "  client: $_" }
$final = @($clientLines | Where-Object { $_ -match 'DEVICESTATUS #' } | Select-Object -Last 1)
Check ($final.Count -eq 1 -and $final[0].Contains("`"name`":`"$newName`"") -and -not $final[0].Contains('"name":"MS300"')) "client: the last deviceStatus has name $newName and no name MS300"
$banner = @($clientLines | Where-Object { $_ -match 'FINAL BANNER' })
Check ($banner.Count -eq 1 -and $banner[0].EndsWith("$newName" + [char]0xFF08 + 'COM2' + [char]0xFF09)) "client: final banner ends with $newName(COM2) and lists no ADAM: $($banner -join ' ')"
while (((Get-Date) - $app.StartTime).TotalSeconds -lt $UptimeSec) { Start-Sleep -Seconds 1 }
$uptime = [int]((Get-Date) - $app.StartTime).TotalSeconds
$sent = $app.CloseMainWindow()
$exited = $app.WaitForExit(30000)
Say "uptime $uptime s; WM_CLOSE sent=$sent exited=$exited exit_code=$(if ($exited) { $app.ExitCode } else { 'n/a' })"
Check ($uptime -ge 45) "app ran $uptime s (>= 45 s)"
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
[System.IO.File]::WriteAllLines((Join-Path $OutDir 'log-devicestatus.txt'), [string[]]$ds, (New-Object System.Text.UTF8Encoding($false)))
$ds | ForEach-Object { Say "  log: $_" }
$first = @($ds | Where-Object { $_ -match ', first result\)' })
Check ($first.Count -eq 6) "log: one first-result line per device ($($first.Count) of 6)"
$offMs300 = @($ds | Where-Object { $_.Contains("[DeviceStatus] $newName COM2 (ms300): OFFLINE") })
$attemptsMs300 = @($lines | Where-Object { $_ -match '\[MS300\] COM2 connection attempt #' })
Check ($offMs300.Count -eq 1 -and $attemptsMs300.Count -ge 5) "log: ms300 written once as '$newName COM2 (ms300): OFFLINE' ($($offMs300.Count)) while $($attemptsMs300.Count) open attempts failed ([MS300] log tag unchanged)"
$named300 = @($ds | Where-Object { $_ -match '\[DeviceStatus\] MS300 ' })
Check ($named300.Count -eq 0) "log: no [DeviceStatus] line names the device MS300 ($($named300.Count))"
foreach ($d in @('ADAM-6256 127\.0\.0\.201 \(adam6256\)', 'ADAM-6217 127\.0\.0\.202 \(adam6217a\)', 'ADAM-6217 127\.0\.0\.203 \(adam6217b\)', 'ADAM-6224 127\.0\.0\.204 \(adam6224\)', 'ADAM-6022 127\.0\.0\.205 \(adam6022\)')) {
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
$qmlErr = @($lines | Where-Object { $_ -match '(?i)qrc:/.*(error|warning)|TypeError|ReferenceError|is not a type|module .* is not installed' })
Check ($qmlErr.Count -eq 0) "log: no QML error/warning lines ($($qmlErr.Count))"
$stillOthers = @($others | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
Check ($stillOthers.Count -eq $others.Count) "other TaidaFlowApp / nginx processes still running: $($stillOthers.Count) of $($others.Count)"
Check ([bool](Get-Process -Id $sim[0].Id -ErrorAction SilentlyContinue)) "Adam60xxSimulator pid $($sim[0].Id) still running"
Say "=== $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })"
exit $(if ($failed) { 1 } else { 0 })

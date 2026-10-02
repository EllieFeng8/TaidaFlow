# w2-086 D5: live run of "offsets applied when storing" with the task's own desktop build, without a
# browser and without UI test (no screenshot, no clicks): the process, its ports, a Proxy Mirror test
# client that writes sensorSettingsSv offsets like the settings page and reads the app's Modbus server and
# REST, the app log and the database.
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-086\tools\live-run.ps1
#            [-OutDir docs\evidence\w2-086\live] [-UptimeSec 75] [-PhaseMs 8000]
# Needs: build\w2-086-desktop\TaidaFlowApp.exe, build\w2-086-wasm (web folder), build\w2-086-run\tools
#        (tools\build-live-tools.bat) and a RUNNING Adam60xxSimulator (shared, never started or stopped here).
#
# * Everything of the task's own: config docs\evidence\w2-086\config.w2-086-sim.json (all listeners on
#   127.0.0.1: Modbus server 5396, http 8395, mirror 18396 / 8396, REST 18395, nginx off), data folder
#   build\w2-086-run\live (deleted at the start), web folder build\w2-086-wasm (TAIDAFLOW_WEB_DIR). Other
#   TaidaFlowApp / nginx / simulator processes are listed and never touched; only the app started here is
#   closed (WM_CLOSE).
# * scripts\safety_probe.ps1 -DeviceProfile simulator with that config must be SAFE before the start.
# * Client scenario (mirror_offset_client): P0 offsets 0 -> P1 pt01 +12.5 kPa, tt02 -1.25 degC, flowMeter
#   +3.4 L/min, pt06 +2000 kPa (clamped), tt04 -200 degC (clamped) -> P2 pt01 -12.5 kPa -> P3 offsets 0;
#   it compares the Modbus server input registers and REST /api/sensor/last with raw PV + offset.
# * Log / database checks (exact, every sample of the run): each "[ModbusServer][Mirror] ... raw=R server=S"
#   and each "[SensorOffset] sensor_data sample with offsets: sN key raw=R stored=S" has S = round(R +
#   offset / scale) clamped 0..65535 with the offsets of the latest "[SensorOffset] offsets in use" line;
#   every sensor_data row (sensor_dump, insert order) equals the sample saved at that "[SQL] Saved Server
#   Input Registers" line (stored values for offset columns, the latest mirrored raw value otherwise);
#   clamp warnings rate limited (at most one per column per 60 s).
# Exit 0 = all checks passed; 1 = a check failed; 2 = refused (not built / port in use / no simulator);
# 3 = safety probe not SAFE.
param(
    [string]$OutDir = "docs\evidence\w2-086\live",
    [int]$UptimeSec = 75,
    [int]$PhaseMs = 8000
)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'summary.txt'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss.fff') $m"; Write-Host $line; Add-Content -Encoding utf8 $summary $line }
Set-Content -Encoding utf8 $summary "=== w2-086 live run $(Get-Date -Format o)"
$config = Join-Path $root 'docs\evidence\w2-086\config.w2-086-sim.json'
$exe = Join-Path $root 'build\w2-086-desktop\TaidaFlowApp.exe'
$webDir = Join-Path $root 'build\w2-086-wasm'
$client = Join-Path $root 'build\w2-086-run\tools\mirror_offset_client.exe'
$dump = Join-Path $root 'build\w2-086-run\tools\sensor_dump.exe'
$dataDir = Join-Path $root 'build\w2-086-run\live'
$ports = @(5396, 8395, 8396, 18395, 18396)
$failed = 0
function Check([bool]$ok, [string]$what) { if ($ok) { Say "PASS $what" } else { Say "FAIL $what"; $script:failed++ } }

foreach ($f in @($exe, $client, $dump, (Join-Path $webDir 'TaidaFlowApp.html'))) { if (-not (Test-Path $f)) { Say "not built: $f"; exit 2 } }
$others = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue)
Say ("other TaidaFlowApp / nginx processes (not touched): " + $(if ($others.Count) { ($others | ForEach-Object { "$($_.ProcessName) $($_.Id) $($_.Path)" }) -join '; ' } else { 'none' }))
$sim = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if (-not $sim.Count) { Say 'Adam60xxSimulator is not running - refused (start it first; it is shared, not started here)'; exit 2 }
Say "Adam60xxSimulator running (pid $($sim[0].Id)) - shared, not stopped"
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
if ($busy.Count) { Say ("ports in use: " + (($busy | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" }) -join '; ') + " - refused"); exit 2 }
Say "ports free: $($ports -join ', ')"
if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
New-Item -ItemType Directory -Force $dataDir | Out-Null
Say "data folder (fresh): $dataDir"

& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason "w2-086 live" `
    -DeviceProfile simulator -Config $config -Exe $exe -LogFile (Join-Path $OutDir 'safety-probe-sim.log') *> (Join-Path $OutDir 'safety-probe.console.txt')
$rc = $LASTEXITCODE
Say "safety_probe exit=$rc ($(if ($rc -eq 0) { 'SAFE' } else { 'NOT SAFE' }))"
if ($rc -ne 0) { exit 3 }

$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_CONFIG = $config
$env:TAIDAFLOW_DEVICE_PROFILE = 'simulator'
$env:TAIDAFLOW_WEB_DIR = $webDir

$appLog = Join-Path $OutDir 'app.stderr.log'
$app = Start-Process -FilePath $exe -WorkingDirectory $dataDir -PassThru -RedirectStandardError $appLog -RedirectStandardOutput "$appLog.stdout"
$null = $app.Handle
Say "app started pid=$($app.Id)"
Start-Sleep -Seconds 10
$listen = @(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue)
Say ("app listeners: " + (($listen | Sort-Object LocalPort | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ' | '))
$own = @($listen | Where-Object { $ports -contains $_.LocalPort })
$notLoopback = @($listen | Where-Object { $_.LocalAddress -ne '127.0.0.1' })
Check ($own.Count -eq 5 -and $notLoopback.Count -eq 0) "app listens on its 5 ports ($($ports -join ', ')), loopback only"
$rest = curl.exe -s -w '|%{http_code}' http://127.0.0.1:18395/
Check ($rest -match '"status":"ok".*\|200$') "REST GET / -> $rest"
$clientOut = Join-Path $OutDir 'client.txt'
$cl = Start-Process -FilePath $client -ArgumentList @('--url', 'ws://127.0.0.1:8396/mirror', '--origin', 'http://127.0.0.1:8395',
        '--rest', 'http://127.0.0.1:18395', '--modbus-port', '5396', '--phase-ms', "$PhaseMs") `
        -PassThru -NoNewWindow -RedirectStandardOutput $clientOut -RedirectStandardError "$clientOut.err"
$null = $cl.Handle
$done = $cl.WaitForExit(180000)
if (-not $done) { Stop-Process -Id $cl.Id -Force }
Say "client exit=$(if ($done) { $cl.ExitCode } else { 'timeout' })"
Check ($done -and $cl.ExitCode -eq 0) "mirror client scenario: every check passed"
$last = curl.exe -s http://127.0.0.1:18395/api/sensor/last
Say "REST /api/sensor/last after the scenario: $last"
while (((Get-Date) - $app.StartTime).TotalSeconds -lt $UptimeSec) { Start-Sleep -Seconds 1 }
$uptime = [int]((Get-Date) - $app.StartTime).TotalSeconds
$sent = $app.CloseMainWindow()
$exited = $app.WaitForExit(30000)
Say "uptime $uptime s; WM_CLOSE sent=$sent exited=$exited exit_code=$(if ($exited) { $app.ExitCode } else { 'n/a' })"
Check ($uptime -ge $UptimeSec) "app ran $uptime s (>= $UptimeSec s)"
Check ($exited -and $app.ExitCode -eq 0) "app closed with exit code 0"
Start-Sleep -Seconds 1
$left = @(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue)
Check ($left.Count -eq 0) "no listener of the app left"
Get-Content $clientOut -Encoding utf8 | Where-Object { $_ -match 'PASS|FAIL|SUMMARY|RESULT|WRITE|MISMATCH|first row' } | ForEach-Object { Say "  client: $_" }

# ---- database (read-only dump, insert order) ----
$dbFile = Join-Path $OutDir 'db-sensor_data.txt'
$p = Start-Process -FilePath $dump -ArgumentList @((Join-Path $dataDir 'data')) -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $dbFile -RedirectStandardError "$dbFile.err"
Say "sensor_dump exit=$($p.ExitCode)"
$dbRows = @(Get-Content $dbFile -Encoding utf8 | ForEach-Object {
    $m = [regex]::Match($_, '^rowid=(\d+) ts=(\d+) time=(\S+ \S+) s=(.*)$')
    if ($m.Success) { [pscustomobject]@{ rowid = [int64]$m.Groups[1].Value; ts = [int64]$m.Groups[2].Value; time = $m.Groups[3].Value; s = @($m.Groups[4].Value -split ',') } }
})
Say "sensor_data rows: $($dbRows.Count)"

# ---- the app's full log: exact replay of every conversion ----
$full = @(Get-ChildItem (Join-Path $dataDir 'logs') -Filter '*-full.log' | Sort-Object LastWriteTime)
$fullText = ($full | ForEach-Object { [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8) }) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $OutDir 'app-full-log.txt'), $fullText, (New-Object System.Text.UTF8Encoding($false)))
$lines = $fullText -split "`r?`n"
Set-Content -Encoding utf8 (Join-Path $OutDir 'log-excerpt.txt') ($lines | Where-Object { $_ -match '\[SensorOffset\]|\[SettingsPage\] (Saved|Loaded) card' })

$keyOfReg = @('tt01', 'tt02', 'tt03', 'tt04', 'pt01', 'pt02', 'pt03', 'pt04', 'pt05', 'pt06', 'pt07', 'flowMeter')
function ScaleOf([string]$key) { if ($key -like 'pt*') { 1000.0 / 65535.0 } elseif ($key -like 'tt*') { 100.0 / 65535.0 } else { 1.0 } }
function Expected([int]$raw, [double]$off, [double]$scale) {
    if ($off -eq 0) { return $raw }
    $v = [math]::Round($raw + $off / $scale, [MidpointRounding]::AwayFromZero)
    if ($v -lt 0) { return 0 }
    if ($v -gt 65535) { return 65535 }
    return [int]$v
}
$offsets = @{}
$lastRaw = @{}
$pending = @{}
$expectedRows = New-Object System.Collections.Generic.List[object]
$mirrorChecked = 0; $mirrorBad = 0; $mirrorWithOffset = 0
$sampleChecked = 0; $sampleBad = 0; $offsetLines = 0
foreach ($l in $lines) {
    if ($l -match '\[SensorOffset\] offsets in use from the next sample: (.*)$') {
        $offsetLines++
        $offsets = @{}
        foreach ($m in [regex]::Matches($Matches[1], '(\w+) (-?\d+\.\d+) \S+ = ')) { $offsets[$m.Groups[1].Value] = [double]$m.Groups[2].Value }
        continue
    }
    $m = [regex]::Match($l, '\[ModbusServer\]\[Mirror\] inputRegister=(\d+) .* raw=(\d+) server=(\d+)')
    if ($m.Success) {
        $reg = [int]$m.Groups[1].Value; $raw = [int]$m.Groups[2].Value; $srv = [int]$m.Groups[3].Value
        $lastRaw[$reg] = $raw
        $exp = $raw
        if ($reg -lt $keyOfReg.Count) {
            $key = $keyOfReg[$reg]
            $off = if ($offsets.ContainsKey($key)) { $offsets[$key] } else { 0.0 }
            if ($off -ne 0) { $mirrorWithOffset++ }
            $exp = Expected $raw $off (ScaleOf $key)
        }
        $mirrorChecked++
        if ($srv -ne $exp) { $mirrorBad++; if ($mirrorBad -le 5) { Say "  mirror mismatch: $l (expected server=$exp)" } }
        continue
    }
    if ($l -match '\[SensorOffset\] sensor_data sample with offsets: (.*)$') {
        foreach ($m in [regex]::Matches($Matches[1], 's(\d+) (\w+) raw=(\d+) stored=(\d+)')) {
            $col = [int]$m.Groups[1].Value; $key = $m.Groups[2].Value; $raw = [int]$m.Groups[3].Value; $st = [int]$m.Groups[4].Value
            $off = if ($offsets.ContainsKey($key)) { $offsets[$key] } else { 0.0 }
            $exp = Expected $raw $off (ScaleOf $key)
            $sampleChecked++
            if ($st -ne $exp -or $lastRaw[$col - 1] -ne $raw -or $keyOfReg[$col - 1] -ne $key) {
                $sampleBad++; if ($sampleBad -le 5) { Say "  sample mismatch: s$col $key raw=$raw stored=$st expected $exp (last mirrored raw $($lastRaw[$col - 1]))" }
            }
            $pending[$col - 1] = $st
        }
        continue
    }
    if ($l -match '\[SQL\] Saved Server Input Registers 0\.\.(\d+) to sensor_data') {
        $row = @()
        for ($r = 0; $r -lt 16; $r++) { $row += $(if ($pending.ContainsKey($r)) { $pending[$r] } elseif ($lastRaw.ContainsKey($r)) { $lastRaw[$r] } else { 0 }) }
        $expectedRows.Add($row)
        $pending = @{}
    }
}
Say "log: $offsetLines offsets-in-use line(s); $mirrorChecked mirrored registers checked ($mirrorWithOffset with an offset); $sampleChecked offset sample values checked; $($expectedRows.Count) saves"
Check ($offsetLines -ge 4) "log: offsets-in-use lines for start / P1 / P2 / P3 ($offsetLines)"
Check ($mirrorChecked -gt 500 -and $mirrorWithOffset -gt 50 -and $mirrorBad -eq 0) "log: every Modbus server PV = round(raw + offset / scale) clamped ($mirrorBad mismatches)"
Check ($sampleChecked -gt 50 -and $sampleBad -eq 0) "log: every stored offset value = round(raw + offset / scale) clamped, raw = the mirrored raw ($sampleBad mismatches)"
$rowBad = 0; $rowsWithOffset = 0
$n = [math]::Min($dbRows.Count, $expectedRows.Count)
for ($i = 0; $i -lt $n; $i++) {
    $diff = @()
    for ($r = 0; $r -lt 16; $r++) {
        if ([double]$dbRows[$i].s[$r] -ne [double]$expectedRows[$i][$r]) { $diff += "s$($r + 1) db=$($dbRows[$i].s[$r]) expected=$($expectedRows[$i][$r])" }
    }
    if ($diff.Count) { $rowBad++; if ($rowBad -le 5) { Say "  row mismatch rowid=$($dbRows[$i].rowid) $($dbRows[$i].time): $($diff -join '; ')" } }
}
Check ($dbRows.Count -ge 40 -and $dbRows.Count -eq $expectedRows.Count -and $rowBad -eq 0) "db: all $($dbRows.Count) sensor_data rows = the logged samples (offset columns stored with the offset of that moment, others raw; $rowBad mismatches, $($expectedRows.Count) saves logged)"
$clampPt06 = @($lines | Where-Object { $_ -match '\[SensorOffset\] pt06 \(s10\) raw=\d+ \+ offset 2000\.000 kPa .* clamped to 65535' })
$clampTt04 = @($lines | Where-Object { $_ -match '\[SensorOffset\] tt04 \(s4\) raw=\d+ \+ offset -200\.000 \S+ .* clamped to 0' })
Check ($clampPt06.Count -ge 1 -and $clampPt06.Count -le 2 -and $clampTt04.Count -ge 1 -and $clampTt04.Count -le 2) "log: clamp warnings rate limited (pt06 $($clampPt06.Count), tt04 $($clampTt04.Count); one per column per 60 s)"
$pvRaw = @($lines | Where-Object { $_ -match '\[Modbus\]\[Read\] device=ADAM-6217 A .* point=PT-01 offset=0 raw=(\d+) value=' })
Check ($pvRaw.Count -gt 20) "log: PT-01 PV still written from the raw count ([Modbus][Read] ... value = raw x scale, $($pvRaw.Count) lines)"
$qmlErr = @($lines | Where-Object { $_ -match '(?i)qrc:/.*(error|warning)|TypeError|ReferenceError|is not a type|module .* is not installed' })
Check ($qmlErr.Count -eq 0) "log: no QML error/warning lines ($($qmlErr.Count))"
$stillOthers = @($others | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
Check ($stillOthers.Count -eq $others.Count) "other TaidaFlowApp / nginx processes still running: $($stillOthers.Count) of $($others.Count)"
Check ([bool](Get-Process -Id $sim[0].Id -ErrorAction SilentlyContinue)) "Adam60xxSimulator pid $($sim[0].Id) still running"
Say "=== $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })"
exit $(if ($failed) { 1 } else { 0 })

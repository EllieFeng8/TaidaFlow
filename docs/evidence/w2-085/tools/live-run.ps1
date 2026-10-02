# w2-085 D5: live run of the settings-page limit alarms with the task's own desktop build, without a
# browser and without UI test (no screenshot, no clicks): the process, its ports, a Proxy Mirror test
# client that writes sensorSettingsSv like the settings page, the app log and the database.
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-085\tools\live-run.ps1
#            [-OutDir docs\evidence\w2-085\live] [-Run1Sec 95] [-Run2Sec 35]
# Needs: build\w2-085-desktop\TaidaFlowApp.exe, build\w2-085-wasm (web folder), build\w2-085-run\tools
#        (tools\build-live-tools.bat) and a RUNNING Adam60xxSimulator (shared, never started or stopped here:
#        scripts\run-simulator.ps1 refuses while another program listens on port 502).
#
# * Everything of the task's own: config docs\evidence\w2-085\config.w2-085-sim.json (all listeners on
#   127.0.0.1: Modbus server 5385, http 8385, mirror 18386 / 8386, REST 18385, nginx off), data folder
#   build\w2-085-run\live (deleted at the start), web folder build\w2-085-wasm (TAIDAFLOW_WEB_DIR, so the
#   runtime.json of build\wasm-release is never written). Other TaidaFlowApp / nginx / simulator processes
#   are listed and never touched; only the app started here is closed (WM_CLOSE).
# * scripts\safety_probe.ps1 -DeviceProfile simulator with that config must be SAFE before each start.
# * Run 1 (>= Run1Sec): client scenario run1 (PT-04 upper, TT-01 lower with a 1 s return, Filter upper,
#   PT-05 upper left open), app closed.  Run 2 (restart, same data folder and settings file): client
#   scenario run2 (row of run 1 taken over, no new row, resolved 2 s after the limit is disabled).
# * Checks: client exit 0 both runs, app exit code 0, log lines ([SQL] Alarm inserted / resolved,
#   [LimitAlarm] ... back in range / again, [LimitAlarm][Restart] ... took over), database rows
#   (alarm_dump), no QML error, no listener left, other processes still running.
# Exit 0 = all checks passed; 1 = a check failed; 2 = refused (not built / port in use / no simulator);
# 3 = safety probe not SAFE.
param(
    [string]$OutDir = "docs\evidence\w2-085\live",
    [int]$Run1Sec = 95,
    [int]$Run2Sec = 35
)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'summary.txt'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss.fff') $m"; Write-Host $line; Add-Content -Encoding utf8 $summary $line }
Set-Content -Encoding utf8 $summary "=== w2-085 live run $(Get-Date -Format o)"
$config = Join-Path $root 'docs\evidence\w2-085\config.w2-085-sim.json'
$exe = Join-Path $root 'build\w2-085-desktop\TaidaFlowApp.exe'
$webDir = Join-Path $root 'build\w2-085-wasm'
$client = Join-Path $root 'build\w2-085-run\tools\mirror_limit_client.exe'
$dump = Join-Path $root 'build\w2-085-run\tools\alarm_dump.exe'
$dataDir = Join-Path $root 'build\w2-085-run\live'
$ports = @(5385, 8385, 8386, 18385, 18386)
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

function Probe([string]$tag) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason "w2-085 live $tag" `
        -DeviceProfile simulator -Config $config -Exe $exe -LogFile (Join-Path $OutDir 'safety-probe-sim.log') *> (Join-Path $OutDir "safety-probe-$tag.console.txt")
    $rc = $LASTEXITCODE
    Say "safety_probe ($tag) exit=$rc ($(if ($rc -eq 0) { 'SAFE' } else { 'NOT SAFE' }))"
    return $rc
}

$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_CONFIG = $config
$env:TAIDAFLOW_DEVICE_PROFILE = 'simulator'
$env:TAIDAFLOW_WEB_DIR = $webDir

function Run-App([string]$tag, [int]$uptimeSec, [string[]]$clientArgs) {
    if ((Probe $tag) -ne 0) { exit 3 }
    $appLog = Join-Path $OutDir "app-$tag.stderr.log"
    $app = Start-Process -FilePath $exe -WorkingDirectory $dataDir -PassThru -RedirectStandardError $appLog -RedirectStandardOutput "$appLog.stdout"
    $null = $app.Handle
    Say "[$tag] app started pid=$($app.Id)"
    Start-Sleep -Seconds 10
    $listen = @(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue)
    Say ("[$tag] app listeners: " + (($listen | Sort-Object LocalPort | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ' | '))
    $own = @($listen | Where-Object { $ports -contains $_.LocalPort })
    $notLoopback = @($listen | Where-Object { $_.LocalAddress -ne '127.0.0.1' })
    Check ($own.Count -eq 5 -and $notLoopback.Count -eq 0) "[$tag] app listens on its 5 ports ($($ports -join ', ')), loopback only"
    $rest = curl.exe -s -w '|%{http_code}' http://127.0.0.1:18385/
    Check ($rest -match '"status":"ok".*\|200$') "[$tag] REST GET / -> $rest"
    $clientOut = Join-Path $OutDir "client-$tag.txt"
    $cl = Start-Process -FilePath $client -ArgumentList (@('--url', 'ws://127.0.0.1:8386/mirror', '--origin', 'http://127.0.0.1:8385') + $clientArgs) `
            -PassThru -NoNewWindow -RedirectStandardOutput $clientOut -RedirectStandardError "$clientOut.err"
    $null = $cl.Handle
    $done = $cl.WaitForExit(150000)
    if (-not $done) { Stop-Process -Id $cl.Id -Force }
    Say "[$tag] client exit=$(if ($done) { $cl.ExitCode } else { 'timeout' })"
    Check ($done -and $cl.ExitCode -eq 0) "[$tag] mirror client scenario: every check passed"
    while (((Get-Date) - $app.StartTime).TotalSeconds -lt $uptimeSec) { Start-Sleep -Seconds 1 }
    $uptime = [int]((Get-Date) - $app.StartTime).TotalSeconds
    $sent = $app.CloseMainWindow()
    $exited = $app.WaitForExit(30000)
    Say "[$tag] uptime $uptime s; WM_CLOSE sent=$sent exited=$exited exit_code=$(if ($exited) { $app.ExitCode } else { 'n/a' })"
    Check ($uptime -ge $uptimeSec) "[$tag] app ran $uptime s (>= $uptimeSec s)"
    Check ($exited -and $app.ExitCode -eq 0) "[$tag] app closed with exit code 0"
    Start-Sleep -Seconds 1
    $left = @(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue)
    Check ($left.Count -eq 0) "[$tag] no listener of the app left"
    return @{ out = $clientOut; log = $appLog }
}

function Dump([string]$tag, [string[]]$more) {
    $file = Join-Path $OutDir "db-$tag.txt"
    # Start-Process keeps the tool's UTF-8 bytes as they are (a PowerShell redirect would re-encode them).
    $p = Start-Process -FilePath $dump -ArgumentList (@((Join-Path $dataDir 'data')) + $more) -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $file -RedirectStandardError "$file.err"
    Say "alarm_dump ($tag) exit=$($p.ExitCode)"
    Get-Content $file -Encoding utf8 | ForEach-Object { Say "  db: $_" }
    return @(Get-Content $file -Encoding utf8)
}

# ---- run 1 ----
$r1 = Run-App 'run1' $Run1Sec @('--scenario', 'run1')
$c1 = @(Get-Content $r1.out -Encoding utf8)
$c1 | Where-Object { $_ -match 'PASS|FAIL|RECORD|LEAVE_OPEN|RESULT|value |WRITE' } | ForEach-Object { Say "  client1: $_" }
$m = [regex]::Match(($c1 -join "`n"), 'LEAVE_OPEN id=(\d+)')
if (-not $m.Success) { Say 'run 1 left no open row - run 2 skipped'; $failed++ }
$openId = if ($m.Success) { $m.Groups[1].Value } else { '-1' }
$db1 = Dump 'after-run1' @()

# ---- run 2 (restart) ----
if ($m.Success) {
    $r2 = Run-App 'run2' $Run2Sec @('--scenario', 'run2', '--open-id', $openId)
    $c2 = @(Get-Content $r2.out -Encoding utf8)
    $c2 | Where-Object { $_ -match 'PASS|FAIL|RECORD|RESULT|value |WRITE' } | ForEach-Object { Say "  client2: $_" }
}
$db2 = Dump 'after-run2' @()

# ---- database ----
$limitRows = @($db2 | Where-Object { $_ -match '^id=\d+ .* limit=(upper|lower) ' })
Check ($limitRows.Count -eq 4) "db: 4 limit alarm rows in total over both runs (PT-04, TT-01, Filter 壓差, PT-05; none added by the restart): $($limitRows.Count)"
foreach ($s in @(@('PT-04', 'upper', '超過上限：'), @('TT-01', 'lower', '低於下限：'), @('Filter 壓差', 'upper', '超過上限：'), @('PT-05', 'upper', '超過上限：'))) {
    $row = @($limitRows | Where-Object { $_ -match (" sensor=" + [regex]::Escape($s[0]) + " status=警告 limit=" + $s[1] + " resolved=1 message=" + [regex]::Escape($s[2])) })
    Check ($row.Count -eq 1) "db: one $($s[0]) $($s[1]) row, status 警告, resolved=1 after run 2: $($row -join ' / ')"
}
$open1 = @($db1 | Where-Object { $_ -match "^id=$openId .* sensor=PT-05 status=警告 limit=upper resolved=0 " })
Check ($open1.Count -eq 1) "db after run 1: PT-05 row id=$openId still open (resolved=0)"

# ---- logs (full log = UTF-8 with time stamps; both runs in this data folder) ----
$full = @(Get-ChildItem (Join-Path $dataDir 'logs') -Filter '*-full.log' | Sort-Object LastWriteTime)
$fullText = ($full | ForEach-Object { [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8) }) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $OutDir 'app-full-log.txt'), $fullText, (New-Object System.Text.UTF8Encoding($false)))
$lines = $fullText -split "`r?`n"
$starts = @(for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '\[Config\] config file ') { $i } })
Check ($starts.Count -eq 2) "log: two runs in the data folder's full log ($($starts.Count))"
$run1 = if ($starts.Count -ge 2) { $lines[$starts[0]..($starts[1] - 1)] } else { $lines }
$run2 = if ($starts.Count -ge 2) { $lines[$starts[1]..($lines.Count - 1)] } else { @() }
$interesting = '\[LimitAlarm\]|\[SQL\] Alarm (inserted|resolved)|\[SettingsPage\] (Saved|Loaded) card|\[Core\] shutdown'
Set-Content -Encoding utf8 (Join-Path $OutDir 'log-excerpt-run1.txt') ($run1 | Where-Object { $_ -match $interesting })
Set-Content -Encoding utf8 (Join-Path $OutDir 'log-excerpt-run2.txt') ($run2 | Where-Object { $_ -match $interesting })
function Has([string[]]$part, [string]$pattern) { return @($part | Where-Object { $_ -match $pattern }).Count }
Check ((Has $run1 '\[LimitAlarm\] settings-page limit alarms active for 13 sensors') -eq 1) 'log run1: monitor active for 13 sensors'
Check ((Has $run1 '\[SQL\] Alarm inserted: sensor=PT-04 message=超過上限：.* status=警告 id=\d+ \(limit alarm pt04 upper\)') -eq 1) 'log run1: PT-04 upper inserted once'
Check ((Has $run1 '\[SQL\] Alarm inserted: sensor=TT-01 message=低於下限：.* status=警告 id=\d+ \(limit alarm tt01 lower\)') -eq 1) 'log run1: TT-01 lower inserted once (also after the 1 s return)'
Check ((Has $run1 '\[LimitAlarm\] TT-01 低於下限 again .* ms after it was back in range: id=\d+ stays unresolved, no new row') -eq 1) 'log run1: TT-01 violated again within 2 s -> same row, no new row'
Check ((Has $run1 '\[SQL\] Alarm inserted: sensor=Filter 壓差 message=超過上限：.*\(limit alarm filter upper\)') -eq 1) 'log run1: Filter 壓差 upper inserted once'
Check ((Has $run1 '\[SQL\] Alarm inserted: sensor=PT-05 message=超過上限：.*\(limit alarm pt05 upper\)') -eq 1) 'log run1: PT-05 upper inserted once'
Check ((Has $run1 '\[SQL\] Alarm resolved: id=\d+ sensor=(PT-04|TT-01|Filter 壓差) message=.* status=警告 detail=(超過上限|低於下限) 解除') -eq 3) 'log run1: three rows resolved (PT-04, TT-01, Filter 壓差)'
Check ((Has $run1 '\[SettingsPage\] Saved card') -ge 4) 'log run1: settings cards saved to the own settings file (Ukai0107 save path unchanged)'
Check ((Has $run2 "\[LimitAlarm\]\[Restart\] PT-05 超過上限: took over id=$openId .*still violated, no new row") -eq 1) "log run2: PT-05 row id=$openId taken over after the restart"
Check ((Has $run2 '\[SQL\] Alarm inserted: sensor=PT-05') -eq 0) 'log run2: no PT-05 row inserted'
Check ((Has $run2 "\[SQL\] Alarm resolved: id=$openId sensor=PT-05 .*detail=超過上限 解除（上限已停用") -eq 1) "log run2: id=$openId resolved after the limit was disabled"
Check ((Has $run2 '\[SettingsPage\] Loaded card .pt04.') -eq 1) 'log run2: PT-04/PT-05 card restored from the settings file'
# timing of the resolves (full log time stamps): back in range -> resolved
$pairs = @()
foreach ($part in @($run1, $run2)) {
    $back = @{}
    foreach ($l in $part) {
        $t = [regex]::Match($l, '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d\d\d)')
        if (-not $t.Success) { continue }
        $time = [datetime]::ParseExact($t.Groups[1].Value, 'yyyy-MM-dd HH:mm:ss.fff', $null)
        $b = [regex]::Match($l, 'back in range .*: id=(\d+) is resolved in')
        if ($b.Success) { $back[$b.Groups[1].Value] = $time }
        $a = [regex]::Match($l, 'again .* id=(\d+) stays unresolved')
        if ($a.Success) { $back.Remove($a.Groups[1].Value) }
        $r = [regex]::Match($l, '\[SQL\] Alarm resolved: id=(\d+) sensor=')
        if ($r.Success -and $back.ContainsKey($r.Groups[1].Value)) {
            $pairs += ('id={0} {1:n0} ms' -f $r.Groups[1].Value, ($time - $back[$r.Groups[1].Value]).TotalMilliseconds)
            $ms = ($time - $back[$r.Groups[1].Value]).TotalMilliseconds
            Check ($ms -ge 1995 -and $ms -le 2300) ("log: id={0} resolved {1:n0} ms after it was back in range (2 s rule)" -f $r.Groups[1].Value, $ms)
        }
    }
}
Check ($pairs.Count -eq 4) "log: 4 back-in-range -> resolved pairs ($($pairs -join ', '))"
$allLog = @($lines)
$qmlErr = @($allLog | Where-Object { $_ -match '(?i)qrc:/.*(error|warning)|TypeError|ReferenceError|is not a type|module .* is not installed' })
Check ($qmlErr.Count -eq 0) "log: no QML error/warning lines ($($qmlErr.Count))"
$warn = @($allLog | Where-Object { $_ -match '(?i)\b(warning|critical|fatal)\b' -or $_ -match '\[W\]|\[C\]' })
$limitWarn = @($allLog | Where-Object { $_ -match 'LimitAlarm' -and $_ -match '(?i)failed|warning' })
Check ($limitWarn.Count -eq 0) "log: no LimitAlarm warning/failure lines ($($limitWarn.Count))"

$stillOthers = @($others | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
Check ($stillOthers.Count -eq $others.Count) "other TaidaFlowApp / nginx processes still running: $($stillOthers.Count) of $($others.Count)"
Check ([bool](Get-Process -Id $sim[0].Id -ErrorAction SilentlyContinue)) "Adam60xxSimulator pid $($sim[0].Id) still running"
Say "=== $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })"
exit $(if ($failed) { 1 } else { 0 })

# w2-071 / w2-072 live check on the DEVELOPMENT PC (Adam60xxSimulator only, no UI interaction).
#
#   -Mode rest     (w2-071 D7) deploy\dev\config.simulator.json as it is, TAIDAFLOW_DEVICE_PROFILE=simulator,
#                  started through scripts\run-desktop.ps1 (its safety probe must say SAFE). >= -RunSec
#                  seconds: curl 127.0.0.1:<rest.port> on the six range routes (paged format), page=4294967297
#                  -> 400, to=9200000000000000 and from=0&to=100000000000000 -> 400 within 1 s; afterwards the
#                  quiet log lines of this run are checked for "Schema file not found" (<= 1).
#   -Mode offline  (w2-072 D6) test config docs\evidence\w2-072\config.offline-adam6217b.json = config.simulator.json
#                  with devices.adam6217b.host 127.0.0.210 (loopback, nobody listens), modbusServer.port 15020
#                  (so that the app's own Modbus server - 0.0.0.0 - does not answer 127.0.0.210:502) and its own
#                  dataDir build\w2-072-run (fresh logs). No TAIDAFLOW_DEVICE_PROFILE (it would replace the
#                  host). Safety gate: the probe of config.simulator.json (-DeviceProfile simulator) must be
#                  SAFE, and the probe of the test config may only report the intended offline device
#                  127.0.0.210:502 as "nobody listening" (verdict SIMULATOR-NOT-READY, nothing UNSAFE / BUSY).
#                  >= -RunSec seconds, then the logs are analysed (analyse-offline in this script).
#
# The simulator is started only when it is not running (scripts\run-simulator.ps1) and then closed
# again at the end; a simulator that was already running is left alone. The app is closed with
# "taskkill /PID <pid>" (WM_CLOSE, no /F). Nothing else is stopped.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-071\tools\live-run.ps1 -Mode rest|offline [-RunSec 95]
# Exit 0 = every check passed; 1 = a check failed; 3 = refused / not started.
param([ValidateSet('rest', 'offline')][string]$Mode = 'rest', [int]$RunSec = 0)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$curl = "$env:SystemRoot\System32\curl.exe"
if ($RunSec -le 0) { $RunSec = if ($Mode -eq 'rest') { 95 } else { 130 } }
$out = if ($Mode -eq 'rest') { Join-Path $root 'docs\evidence\w2-071\live' } else { Join-Path $root 'docs\evidence\w2-072\live' }
New-Item -ItemType Directory -Force $out | Out-Null
$summary = Join-Path $out 'summary.txt'
if (Test-Path $summary) { Remove-Item $summary }
$failed = 0
function Note([string]$m) { Add-Content -Path $summary -Value $m -Encoding utf8; [Console]::Out.WriteLine($m) }
function Check([bool]$ok, [string]$what) { if ($ok) { Note "PASS  $what" } else { Note "FAIL  $what"; $script:failed++ } }
# A script that starts a long-running process (simulator, app) is run through "cmd /c ... > file": the
# started process would otherwise inherit the output pipe and the call would never return.
function Invoke-Script([string]$script, [string]$argText, [string]$console) {
    $p = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -WindowStyle Hidden -PassThru -ArgumentList ('/c ""' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + $script + '" ' + $argText + ' > "' + $console + '" 2>&1"')
    $null = $p.Handle
    if (-not $p.WaitForExit(180000)) { return "timeout" }
    return ((Get-Content -LiteralPath $console) -join "`n")
}

Note ("=== live-run mode={0} start={1} runSec={2} exe={3}" -f $Mode, (Get-Date -Format o), $RunSec, $exe)
if (-not (Test-Path $exe)) { Note 'exe not found'; exit 3 }
if (Get-Process TaidaFlowApp -ErrorAction SilentlyContinue) { Note 'TaidaFlowApp already running - nothing started'; exit 3 }

$simPid = 0
$sim = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($sim.Count) {
    Note ("Adam60xxSimulator already running (pid {0}) - used as it is, NOT closed by this script" -f (($sim | ForEach-Object { $_.Id }) -join ','))
} else {
    $simText = (Invoke-Script (Join-Path $root 'scripts\run-simulator.ps1') ('-LogFile "' + (Join-Path $out 'simulator.log') + '"') (Join-Path $out 'run-simulator.console.txt')).Trim()
    Note "run-simulator: $simText"
    if ($simText -cmatch '\bSIM_PID=(\d+)') { $simPid = [int]$Matches[1] } else { Note 'simulator not started'; exit 3 }
}

$appPid = 0
$appStart = Get-Date
try {
    $base = Join-Path $root 'deploy\dev\config.simulator.json'
    if ($Mode -eq 'rest') {
        $cfgPath = $base
        $dataDir = Join-Path $root 'build\runtime-cwd'
        $restPort = 18080
        $quiet = Join-Path $dataDir ('logs\taidaflow-{0}.log' -f (Get-Date -Format 'yyyy-MM-dd'))
        $quietBefore = if (Test-Path $quiet) { @(Get-Content -LiteralPath $quiet -Encoding utf8).Count } else { 0 }
        Note "quiet log $quiet had $quietBefore line(s) before this run"
        $appStart = Get-Date
        $rdText = Invoke-Script (Join-Path $root 'scripts\run-desktop.ps1') `
                    ('-Label "w2-071 live REST" -Config "' + $cfgPath + '" -DeviceProfile simulator -LogFile "' + (Join-Path $out 'app.stderr.log') + '" -ProbeLog "' + (Join-Path $out 'safety-probe.log') + '"') `
                    (Join-Path $out 'run-desktop.console.txt')
        Check ($rdText -match 'verdict: SAFE') 'safety_probe (config.simulator.json, -DeviceProfile simulator) verdict SAFE before the launch'
        if ($rdText -cmatch '\bPID=(\d+)') { $appPid = [int]$Matches[1] } else { Note "app not started: $rdText"; exit 3 }
    } else {
        $cfgPath = Join-Path $root 'docs\evidence\w2-072\config.offline-adam6217b.json'
        $dataDir = Join-Path $root 'build\w2-072-run'
        if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
        New-Item -ItemType Directory -Force $dataDir | Out-Null
        $txt = Get-Content -Raw -LiteralPath $base
        $txt = $txt -replace '"dataDir"\s*:\s*"[^"]*"', '"dataDir": "../../../build/w2-072-run"'
        $txt = $txt -replace '("adam6217b"\s*:\s*\{\s*"host"\s*:\s*)"127\.0\.0\.203"', '${1}"127.0.0.210"'
        $txt = $txt -replace '("modbusServer"\s*:\s*\{[^}]*"port"\s*:\s*)502', '${1}15020'
        [System.IO.File]::WriteAllText($cfgPath, $txt, (New-Object System.Text.UTF8Encoding($false)))
        $hosts = [regex]::Matches($txt, '"host"\s*:\s*"([^"]+)"') | ForEach-Object { $_.Groups[1].Value }
        Note ("test config {0}: device hosts {1}" -f $cfgPath, ($hosts -join ', '))
        Check (@($hosts | Where-Object { $_ -notlike '127.*' }).Count -eq 0) 'test config: every device host is 127.x'
        Check ($txt -match '"adam6217b"\s*:\s*\{\s*"host"\s*:\s*"127\.0\.0\.210"') 'test config: devices.adam6217b.host = 127.0.0.210'
        $restPort = 18080
        # Gate 1: the simulator config (every hardware check) must be SAFE.
        $p1 = & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\safety_probe.ps1') -Reason 'w2-072 live gate 1' `
                 -Config $base -DeviceProfile simulator -LogFile (Join-Path $out 'safety-probe.log')
        $p1rc = $LASTEXITCODE
        Check ($p1rc -eq 0 -and (($p1 | Out-String) -match 'verdict: SAFE')) "gate 1: safety_probe config.simulator.json -DeviceProfile simulator -> exit $p1rc (SAFE)"
        # Gate 2: the test config itself: only the intended offline device may be "nobody listening".
        $p2 = & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\safety_probe.ps1') -Reason 'w2-072 live gate 2 (test config)' `
                 -Config $cfgPath -DeviceProfile default -LogFile (Join-Path $out 'safety-probe.log')
        $p2rc = $LASTEXITCODE
        $p2text = ($p2 | Out-String)
        $notReady = @($p2 | Where-Object { $_ -match 'SIMULATOR NOT READY' })
        $gate2 = ($p2rc -eq 0) -or ($p2rc -eq 5 -and $notReady.Count -eq 1 -and $notReady[0] -match '127\.0\.0\.210:502' -and $p2text -notmatch 'UNSAFE' -and $p2text -notmatch '-> BUSY|verdict: BUSY')
        Check $gate2 ("gate 2: safety_probe test config -> exit $p2rc; not-ready endpoints: " + (($notReady | ForEach-Object { $_.Trim() }) -join ' | ') + '; nothing UNSAFE / BUSY')
        if ($failed) { Note 'safety gate not passed - app NOT started'; exit 3 }
        $env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
        $env:TAIDAFLOW_CONFIG = $cfgPath
        $env:QT_FORCE_STDERR_LOGGING = '1'
        Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue
        $appStart = Get-Date
        $p = Start-Process -FilePath $exe -WorkingDirectory $dataDir -PassThru `
                -RedirectStandardError (Join-Path $out 'app.stderr.log') -RedirectStandardOutput (Join-Path $out 'app.stdout.log')
        $null = $p.Handle
        $appPid = $p.Id
        Note "app pid $appPid config $cfgPath"
    }

    # Wait for the REST API.
    $deadline = (Get-Date).AddSeconds(60)
    $up = $false
    do {
        Start-Sleep -Milliseconds 500
        $code = & $curl -s -o NUL -w '%{http_code}' --max-time 2 "http://127.0.0.1:$restPort/"
        $up = ($code -eq '200')
    } while (-not $up -and (Get-Date) -lt $deadline -and (Get-Process -Id $appPid -ErrorAction SilentlyContinue))
    Check $up "REST 127.0.0.1:$restPort answers GET / (after $([int]((Get-Date) - $appStart).TotalSeconds) s)"

    if ($Mode -eq 'rest' -and $up) {
        Start-Sleep -Seconds 5   # a few saved rows
        $now = [DateTimeOffset]::Now.ToUnixTimeSeconds()
        $from = $now - 7 * 86400
        $isoFrom = [DateTimeOffset]::FromUnixTimeSeconds($from).LocalDateTime.ToString('yyyy-MM-ddTHH:mm:ss')
        $isoTo = [DateTimeOffset]::FromUnixTimeSeconds($now).LocalDateTime.ToString('yyyy-MM-ddTHH:mm:ss')
        $curlLog = Join-Path $out 'curl-results.txt'
        if (Test-Path $curlLog) { Remove-Item $curlLog }
        function Get-Api([string]$pathQuery) {
            $body = Join-Path $out 'curl-body.tmp'
            $meta = & $curl -s -o $body -w '%{http_code} %{time_total}' --max-time 30 ("http://127.0.0.1:{0}{1}" -f $restPort, $pathQuery)
            $parts = "$meta".Split(' ')
            $text = if (Test-Path $body) { [System.IO.File]::ReadAllText($body) } else { '' }
            $short = if ($text.Length -gt 300) { $text.Substring(0, 300) + '...' } else { $text }
            Add-Content -Path $curlLog -Value ("GET {0} -> {1} in {2} s: {3}" -f $pathQuery, $parts[0], $parts[1], $short) -Encoding utf8
            $obj = $null
            try { $obj = $text | ConvertFrom-Json } catch { }
            return [pscustomobject]@{ Code = [int]$parts[0]; Secs = [double]::Parse($parts[1], [Globalization.CultureInfo]::InvariantCulture); Obj = $obj; Text = $text }
        }
        $routes = @(
            @{ p = '/api/sensor/range'; q = "from=$from&to=$now" }, @{ p = '/api/holding/range'; q = "from=$from&to=$now" },
            @{ p = '/api/sensor/rangeDateTime'; q = "from=$isoFrom&to=$isoTo" }, @{ p = '/api/holding/rangeDateTime'; q = "from=$isoFrom&to=$isoTo" },
            @{ p = '/api/sensor/rangeDateTimePage'; q = "from=$isoFrom&to=$isoTo" }, @{ p = '/api/holding/rangeDateTimePage'; q = "from=$isoFrom&to=$isoTo" })
        $keys = 'hasNextPage,hasPreviousPage,items,page,pageSize,totalCount,totalPages'
        foreach ($r in $routes) {
            $a = Get-Api ("{0}?{1}" -f $r.p, $r.q)
            $k = if ($a.Obj) { (@($a.Obj.PSObject.Properties.Name) | Sort-Object) -join ',' } else { '' }
            Check ($a.Code -eq 200 -and $k -eq $keys -and $a.Obj.page -eq 1 -and $a.Obj.pageSize -eq 200) `
                  ("{0} default -> {1}, keys [{2}], page {3}, pageSize {4}, totalCount {5}, items {6}" -f $r.p, $a.Code, $k, $a.Obj.page, $a.Obj.pageSize, $a.Obj.totalCount, @($a.Obj.items).Count)
            $b = Get-Api ("{0}?{1}&page=2&pageSize=5000" -f $r.p, $r.q)
            Check ($b.Code -eq 200 -and $b.Obj.pageSize -eq 1000 -and $b.Obj.page -eq 2) ("{0} page=2&pageSize=5000 -> {1}, pageSize {2} (clamped to 1000)" -f $r.p, $b.Code, $b.Obj.pageSize)
            $c = Get-Api ("{0}?{1}&page=4294967297" -f $r.p, $r.q)
            Check ($c.Code -eq 400) ("{0} page=4294967297 -> {1} {2}" -f $r.p, $c.Code, $c.Text)
            $d = Get-Api ("{0}?from=0&to=9200000000000000" -f $r.p)
            Check ($d.Code -eq 400 -and $d.Secs -lt 1.0) ("{0} from=0&to=9200000000000000 -> {1} in {2} s" -f $r.p, $d.Code, $d.Secs)
            $e = Get-Api ("{0}?from=0&to=100000000000000" -f $r.p)
            Check ($e.Code -eq 400 -and $e.Secs -lt 1.0) ("{0} from=0&to=100000000000000 -> {1} in {2} s" -f $r.p, $e.Code, $e.Secs)
        }
        # Sensor rows of the simulator run are there and the same through range and rangeDateTimePage.
        $to2 = $now - 2   # rows of the last seconds may still be saved between the two requests
        $s1 = Get-Api "/api/sensor/range?from=$from&to=$to2&pageSize=1000"
        $s2 = Get-Api "/api/sensor/rangeDateTimePage?from=$from&to=$to2&pageSize=1000"
        Check ($s1.Obj.totalCount -gt 0 -and $s1.Obj.totalCount -eq $s2.Obj.totalCount -and (($s1.Obj.items | ConvertTo-Json -Compress -Depth 5) -eq ($s2.Obj.items | ConvertTo-Json -Compress -Depth 5))) `
              ("sensor rows present ({0}) and identical through /api/sensor/range and /api/sensor/rangeDateTimePage" -f $s1.Obj.totalCount)
        Remove-Item (Join-Path $out 'curl-body.tmp') -ErrorAction SilentlyContinue
    }

    # Keep running for RunSec in total.
    $left = $RunSec - [int]((Get-Date) - $appStart).TotalSeconds
    if ($left -gt 0) { Start-Sleep -Seconds $left }
    Check ([bool](Get-Process -Id $appPid -ErrorAction SilentlyContinue)) ("app still running after {0} s" -f [int]((Get-Date) - $appStart).TotalSeconds)
} finally {
    if ($appPid) {
        $app = Get-Process -Id $appPid -ErrorAction SilentlyContinue
        if ($app) {
            $tk = & "$env:SystemRoot\System32\taskkill.exe" /PID $appPid 2>&1
            Note ("taskkill /PID {0} (no /F): {1}" -f $appPid, ($tk | Out-String).Trim())
            $null = $app.WaitForExit(60000)
            Note ("app exited: {0}, exit code {1}" -f $app.HasExited, $(if ($app.HasExited) { $app.ExitCode } else { '-' }))
        }
    }
    if ($simPid) {
        $s = Get-Process -Id $simPid -ErrorAction SilentlyContinue
        if ($s -and $s.ProcessName -eq 'Adam60xxSimulator') {
            $tk = & "$env:SystemRoot\System32\taskkill.exe" /PID $simPid 2>&1
            $null = $s.WaitForExit(15000)
            Note ("simulator pid {0} (started by this script) closed with taskkill (no /F): {1}; exited {2}" -f $simPid, ($tk | Out-String).Trim(), $s.HasExited)
        }
    }
}

# Logs of this run.
$day = (Get-Date -Format 'yyyy-MM-dd')
if ($Mode -eq 'rest') {
    $lines = if (Test-Path $quiet) { @(Get-Content -LiteralPath $quiet -Encoding utf8) } else { @() }
    $new = @($lines | Select-Object -Skip $quietBefore)
    Set-Content -Path (Join-Path $out 'quiet-log-this-run.txt') -Value $new -Encoding utf8
    $schema = @($new | Where-Object { $_ -match 'Schema file not found' })
    Note ("quiet log lines written by this run: {0}; 'Schema file not found': {1}" -f $new.Count, $schema.Count)
    Check ($schema.Count -le 1) "'Schema file not found' <= 1 line in the quiet log of this run"
    $full = Join-Path $dataDir "logs\taidaflow-$day-full.log"
    if (Test-Path $full) {
        $fl = @(Get-Content -LiteralPath $full -Encoding utf8 | Where-Object { $_ -match '\[REST\] route|Schema file|\[SQL\] SqlManager stopped|\[Core\] shutdown' })
        Set-Content -Path (Join-Path $out 'full-log-excerpt.txt') -Value ($fl | Select-Object -Last 40) -Encoding utf8
    }
} else {
    $logDir = Join-Path $dataDir 'logs'
    Copy-Item (Join-Path $logDir "taidaflow-$day.log") (Join-Path $out 'quiet.log') -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $logDir "taidaflow-$day-full.log") (Join-Path $out 'full.log') -ErrorAction SilentlyContinue
    & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'analyse-offline.ps1') -Dir $out -RunSec $RunSec
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
Note ("=== result: {0} failed check(s)" -f $failed)
if ($failed) { exit 1 } else { exit 0 }

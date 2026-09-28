# w2-067: copy of docs\evidence\w2-065\tools\live-run.ps1 (w2-065) with its own run folder build\w2-067-live and
# evidence folder docs\evidence\w2-067\50-live-run; the app exit code after WM_CLOSE (stop-taidaflow) is a CHECK here
# (must be 0 with the simulator connected), not only information. Everything else unchanged.
# w2-065 live run (DEVELOPMENT PC, simulator only): the package's start-taidaflow.ps1 / stop-taidaflow.ps1
# with a copy of deploy\dev\config.simulator.json, to check the log changes end to end. No UI interaction.
#
#   1. build\w2-067-live\config.json = deploy\dev\config.simulator.json with dataDir "data", modbusServer.port
#      15020 (the simulator itself listens on 127.0.0.201..205:502), nginx.exe "nginx\nginx.exe" (relative to
#      the folder of config.json -> build\w2-067-live\nginx, a copy of the package's nginx: the installed nginx
#      is never used or configured) and log.full.keepDays "seven" (invalid -> default 7 + a warning, so the
#      quiet file has a known line). The log folder is pre-seeded with expired / kept / foreign files.
#   2. scripts\run-simulator.ps1 (Adam60xxSimulator, 127.0.0.201..205:502), then scripts\safety_probe.ps1 with
#      the same config.json: only "verdict: SAFE" continues (otherwise nothing of TaidaFlow is started).
#   3. <package>\start-taidaflow.ps1 -Config <run config> (cmd, PATH without Qt, output to a file), a few
#      requests through nginx (curl), checks, <package>\stop-taidaflow.ps1, "nginx -s quit" in the nginx
#      folder it was started from, the simulator closed. Only processes started here are closed.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-067\tools\live-run.ps1 -Package dist\TaidaFlow-<...>
# Exit 0 = every check passed; 1 = a check failed; 3 = probe not SAFE / something already running (nothing started).
param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
. (Join-Path $root 'scripts\taidaflow-config.ps1')
if (-not [System.IO.Path]::IsPathRooted($Package)) { $Package = Join-Path $root $Package }
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
$ev = Join-Path $root 'docs\evidence\w2-067\50-live-run'
if (Test-Path $ev) { Remove-Item -LiteralPath $ev -Recurse -Force }
New-Item -ItemType Directory -Force $ev | Out-Null
$run = Join-Path $root 'build\w2-067-live'
$cfgPath = Join-Path $run 'config.json'
$logDir = Join-Path $run 'data\logs'
$lines = New-Object System.Collections.Generic.List[string]
$script:fails = 0
function Note([string]$m) { $lines.Add($m); [Console]::Out.WriteLine($m) }
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    if (-not $ok) { $script:fails++ }
    Note ("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' }))
}
function Save { [System.IO.File]::WriteAllLines((Join-Path $ev 'report.txt'), $lines.ToArray(), (New-Object System.Text.UTF8Encoding($false))) }
$cleanPath = @("$env:SystemRoot\System32", "$env:SystemRoot", "$env:SystemRoot\System32\Wbem", "$env:SystemRoot\System32\WindowsPowerShell\v1.0") -join ';'
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
# cmd through ShellExecute (no handle inheritance), Qt-free PATH, output to a file.
function Invoke-Cmd([string]$cmdLine, [string]$outFile) {
    $saved = $env:PATH; $env:PATH = $cleanPath
    Remove-Item Env:\TAIDAFLOW_CONFIG, Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue
    try {
        $p = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList ('/c "' + $cmdLine + ' > "' + $outFile + '" 2>&1"') -WindowStyle Hidden -PassThru
        $null = $p.Handle
        $rc = if ($p.WaitForExit(180000)) { $p.ExitCode } else { 'TIMEOUT' }
    } finally { $env:PATH = $saved }
    Get-Content -LiteralPath $outFile -ErrorAction SilentlyContinue | ForEach-Object { Note "  | $_" }
    return $rc
}
function Get-HttpCode([string]$url) { return (& "$env:SystemRoot\System32\curl.exe" --noproxy '*' -s -o NUL -w '%{http_code}' --max-time 10 $url) }

Note "=== w2-065 live run $((Get-Date).ToString('o'))"
Note "package: $Package"
$pre = @(Get-Process | Where-Object { $_.ProcessName -in 'TaidaFlowApp', 'nginx', 'Adam60xxSimulator' })
foreach ($p in $pre) { Note "already running (NOT touched): $($p.ProcessName) pid $($p.Id)" }
if ($pre.Count) { Note 'something of ours already runs - nothing started'; Save; exit 3 }

# --- 1. run folder -------------------------------------------------------------------------------------
if (Test-Path $run) { Remove-Item -LiteralPath $run -Recurse -Force }
New-Item -ItemType Directory -Force $run, (Join-Path $run 'nginx\conf'), $logDir | Out-Null
Copy-Item -LiteralPath (Join-Path $Package 'nginx\nginx.exe') -Destination (Join-Path $run 'nginx')
Copy-Item -LiteralPath (Join-Path $Package 'nginx\conf\mime.types') -Destination (Join-Path $run 'nginx\conf')
$text = [System.IO.File]::ReadAllText((Join-Path $root 'deploy\dev\config.simulator.json'))
$text = $text.Replace('"dataDir": "../../build/runtime-cwd"', '"dataDir": "data"')
$text = [regex]::Replace($text, '("modbusServer": \{\s*"bind": "0\.0\.0\.0",\s*"port": )502', '${1}15020')
$text = [regex]::Replace($text, '"exe": "[^"]*"', '"exe": "nginx\\nginx.exe"')   # .NET replacement: only $ is special
$text = [regex]::Replace($text, '("full": \{\s*"enabled": true,\s*"keepDays": )7', '${1}"seven"')
[System.IO.File]::WriteAllText($cfgPath, $text, (New-Object System.Text.UTF8Encoding($false)))
Copy-Item -LiteralPath $cfgPath -Destination (Join-Path $ev 'run-config.json')
$cfg = Get-TaidaFlowConfig -Path $cfgPath -Exe (Join-Path $Package 'TaidaFlowApp.exe')
Check 'run config: dataDir data, modbusServer.port 15020, devices 127.0.0.201..205, nginx.exe nginx\nginx.exe (file), log.full.keepDays invalid -> 7' (
    $cfg.DataDir -eq (Join-Path $run 'data') -and $cfg.Values['modbusServer.port'] -eq 15020 -and $cfg.Values['devices.adam6256.host'] -eq '127.0.0.201' -and
    $cfg.Values['nginx.exe'] -eq 'nginx\nginx.exe' -and $cfg.Sources['nginx.exe'] -eq 'file' -and $cfg.Values['log.full.keepDays'] -eq 7 -and
    @($cfg.Notes | Where-Object { $_ -match 'WARNING: "log\.full\.keepDays"' }).Count -eq 1) (($cfg.Notes) -join ' | ')
Check 'nginx.exe resolved against the folder of config.json (like AppConfig::resolvedNginxExe)' ((Resolve-TaidaFlowNginxExe $cfg) -eq (Join-Path $run 'nginx\nginx.exe')) (Resolve-TaidaFlowNginxExe $cfg)
Check 'log.dir resolved against dataDir (like AppConfig::resolvedLogDir)' ((Resolve-TaidaFlowLogDir $cfg) -eq $logDir) (Resolve-TaidaFlowLogDir $cfg)

$today = (Get-Date).Date
function D([int]$daysBack) { return $today.AddDays(-$daysBack).ToString('yyyy-MM-dd') }
$seed = [ordered]@{
    'launcher.log' = 'kept (old launcher log name)'; 'taidaflow-20260801-101010.log' = 'kept (old start script)'; 'taidaflow-20260801-101010.log.stdout' = 'kept (old start script)'
    'my notes.txt' = 'kept (not ours)'; 'nginx-error.log' = 'kept (fixed name, appended by nginx)'
    "launcher-$(D 60).log" = 'deleted by start-taidaflow (60 days: oldest kept is today-59)'; "launcher-$(D 59).log" = 'kept (boundary, 60th day)'
    "nginx-access-$(D 60).log" = 'deleted by start-taidaflow'; "nginx-access-$(D 45).log" = 'kept'
    "taidaflow-$(D 60).log" = 'deleted by the app (quiet 60 days)'; "taidaflow-$(D 59).log" = 'kept by the app'
    "taidaflow-$(D 7)-full.log" = 'deleted by the app (full 7 days)'; "taidaflow-$(D 6)-full.log" = 'kept by the app'
}
foreach ($k in $seed.Keys) { [System.IO.File]::WriteAllText((Join-Path $logDir $k), "w2-065 pre-seeded file ($($seed[$k]))`r`n") }
Note "pre-seeded log folder $logDir :"
foreach ($k in $seed.Keys) { Note "  $k : $($seed[$k])" }

# --- 2. simulator + safety probe ---------------------------------------------------------------------------
# Through cmd with the output in a FILE: the simulator inherits run-simulator's output handle, so its
# output must never be a pipe of this script (the pipe would stay open while the simulator runs).
$simCon = Join-Path $ev 'run-simulator.console.txt'
$saved = $env:PATH
$sp = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -WindowStyle Hidden -PassThru -ArgumentList ('/c ""' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $root 'scripts\run-simulator.ps1') + '" -LogFile "' + (Join-Path $run 'simulator.log') + '" > "' + $simCon + '" 2>&1"')
$null = $sp.Handle
$simRc = if ($sp.WaitForExit(60000)) { $sp.ExitCode } else { 'TIMEOUT' }
$simOut = @(Get-Content -LiteralPath $simCon -ErrorAction SilentlyContinue)
$simOut | ForEach-Object { Note "  sim | $_" }
$simPid = if ("$simOut" -match 'SIM_PID=(\d+)') { [int]$Matches[1] } else { 0 }
function Stop-Sim {
    if ($simPid) {
        $s = Get-Process -Id $simPid -ErrorAction SilentlyContinue
        if ($s -and $s.ProcessName -eq 'Adam60xxSimulator') {
            $null = $s.CloseMainWindow()
            if (-not $s.WaitForExit(10000)) { Stop-Process -Id $simPid -Force; Note "simulator pid $simPid did not close its window in 10 s - stopped (it was started by this script)" }
            Note "simulator pid $simPid closed"
        }
    }
}
if ($simRc -ne 0 -or -not $simPid) { Note "simulator not started (exit $simRc) - nothing else started"; Save; exit 3 }
# Any exception from here on: close what this script started (app via stop-taidaflow, its nginx, the simulator).
trap {
    Note "EXCEPTION: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber)) - cleaning up"
    $null = Invoke-Cmd ('"' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $Package 'stop-taidaflow.ps1') + '" -Config "' + $cfgPath + '"') (Join-Path $ev 'cleanup-stop.console.txt')
    $nx = Join-Path $run 'nginx\nginx.exe'
    if (@(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -eq $nx } catch { $false } }).Count) {
        $q = Start-Process -FilePath $nx -ArgumentList @('-s', 'quit') -WorkingDirectory (Join-Path $run 'nginx') -WindowStyle Hidden -PassThru; $null = $q.WaitForExit(20000)
    }
    Stop-Sim; Save; exit 1
}
$probeOut = & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\safety_probe.ps1') -Reason 'w2-065 live run (package start script, simulator config)' -Config $cfgPath -Exe (Join-Path $Package 'TaidaFlowApp.exe') -LogFile (Join-Path $ev 'safety-probe.log') 2>&1 | ForEach-Object { "$_" }
$probeRc = $LASTEXITCODE
$probeOut | ForEach-Object { Note "  probe | $_" }
Check 'safety probe verdict SAFE (exit 0)' ($probeRc -eq 0 -and "$probeOut" -match 'verdict: SAFE') "exit $probeRc"
if ($probeRc -ne 0) { Stop-Sim; Save; exit 3 }

# --- 3. start (package script), requests, checks ---------------------------------------------------------------
$startOut = Join-Path $ev 'start.console.txt'
$rc = Invoke-Cmd ('"' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $Package 'start-taidaflow.ps1') + '" -Config "' + $cfgPath + '"') $startOut
Check 'start-taidaflow.ps1 -Config <run config>: exit 0' ("$rc" -eq '0') "exit $rc"
$state = try { Get-Content -Raw (Join-Path $run 'data\taidaflow-app.json') | ConvertFrom-Json } catch { $null }
$app = if ($state) { Get-Process -Id ([int]$state.pid) -ErrorAction SilentlyContinue } else { $null }
Check 'TaidaFlowApp running (state file), log folder in the state file' ([bool]$app -and [string]$state.logDir -eq $logDir) $(if ($state) { "pid $($state.pid), logDir $($state.logDir)" } else { 'no state file' })
$ngxExe = Join-Path $run 'nginx\nginx.exe'
$ngx = @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -eq $ngxExe } catch { $false } })
Check 'nginx of build\w2-067-live\nginx started (config.json nginx.exe relative to the config folder)' ($ngx.Count -gt 0) (($ngx | ForEach-Object { $_.Id }) -join ', ')
Start-Sleep -Seconds 3
$req = [ordered]@{ '/' = '302'; '/TaidaFlowApp.html' = '200'; '/runtime.json' = '200'; '/api/' = '200'; '/w2-065-missing.txt' = '404' }
foreach ($u in $req.Keys) { $c = Get-HttpCode "http://127.0.0.1$u"; Check "GET http://127.0.0.1$u through nginx -> $($req[$u])" ($c -eq $req[$u]) $c }
Start-Sleep -Seconds 2

$d0 = $today.ToString('yyyy-MM-dd')
$ll = Join-Path $logDir "launcher-$d0.log"; $full = Join-Path $logDir "taidaflow-$d0-full.log"; $quiet = Join-Path $logDir "taidaflow-$d0.log"; $acc = Join-Path $logDir "nginx-access-$d0.log"
$lt = if (Test-Path $ll) { (Get-Content -LiteralPath $ll -Encoding UTF8) -join "`n" } else { '' }
Check "launcher-$d0.log in the log folder: start lines, 'started (exit 0)'" ($lt -match '\[start\] === TaidaFlow FIELD start' -and $lt -match '\[start\] started \(exit 0\)')
Check 'launcher log: nginx.exe from config.json (relative, file)' ($lt.Contains("nginx: $ngxExe (config.json nginx.exe = nginx\nginx.exe, file"))
Check 'launcher log: clean-up of launcher / nginx access logs (1 expired each deleted), note about the old start script files' ($lt -match 'clean-up launcher log: .* 1 deleted' -and $lt -match 'clean-up nginx access log: .* 1 deleted' -and $lt -match 'note: 2 log file\(s\) of the older start script')
Check 'launcher log: what the app wrote to stderr / stdout during start-up is reported (line counts)' ($lt -match 'app stderr during start-up: \d+ line\(s\)' -and $lt -match 'app stdout during start-up: \d+ line\(s\)') (([regex]::Matches($lt, 'app std(err|out) during start-up: \d+ line\(s\)') | ForEach-Object { $_.Value }) -join '; ')
Check 'launcher log: no QT_LOGGING_CONF, no taidaflow-<time>.log redirection' ($lt -notmatch 'QT_LOGGING_CONF=' -and $lt -notmatch 'taidaflow-\d{8}-\d{6}\.log')
$ft = if (Test-Path $full) { @(Get-Content -LiteralPath $full -Encoding UTF8) } else { @() }
$info = @($ft | Where-Object { $_ -match '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[info\] ' }).Count
Check "full log taidaflow-$d0-full.log has [info] lines" ($info -gt 0) "$($ft.Count) line(s), $info info"
Check 'full log: the invalid log.full.keepDays warning, log.dir (resolved), AppLog clean-up / log folder lines' (@($ft | Where-Object { $_ -match '\[warning\] \[Config\] "log\.full\.keepDays" = "seven" is invalid' }).Count -eq 1 -and @($ft | Where-Object { $_.Contains("log.dir (resolved) = $logDir") }).Count -eq 1 -and @($ft | Where-Object { $_ -match '\[AppLog\] log folder' }).Count -ge 1)
$qt = if (Test-Path $quiet) { @(Get-Content -LiteralPath $quiet -Encoding UTF8) } else { $null }
$qBad = if ($null -ne $qt) { @($qt | Where-Object { $_ -match '^\d{4}-\d{2}-\d{2} ' -and $_ -notmatch '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[(warning|critical|fatal)\] ' }) } else { @('missing') }
$levels = if ($null -ne $qt) { ($qt | ForEach-Object { if ($_ -match '^\S+ \S+ \[(\w+)\]') { $Matches[1] } } | Group-Object | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', ' } else { 'missing' }
Check "quiet log taidaflow-$d0.log: only warning / critical / fatal, with the log.full.keepDays warning" ($null -ne $qt -and $qBad.Count -eq 0 -and @($qt | Where-Object { $_ -match '"log\.full\.keepDays" = "seven"' }).Count -eq 1) $levels
$at = if (Test-Path $acc) { @(Get-Content -LiteralPath $acc) } else { @() }
Check "nginx-access-$d0.log (named by date) in the log folder with the requests above" (@($at | Where-Object { $_ -match '"GET /TaidaFlowApp\.html HTTP/1\.1" 200 ' }).Count -ge 1 -and @($at | Where-Object { $_ -match '"GET /w2-065-missing\.txt HTTP/1\.1" 404 ' }).Count -ge 1) "$($at.Count) line(s)"
Check 'nginx-error.log in the log folder, the pre-seeded content still there (never deleted)' ((Test-Path (Join-Path $logDir 'nginx-error.log')) -and ((Get-Content -Raw (Join-Path $logDir 'nginx-error.log')) -match 'w2-065 pre-seeded file'))
Check 'nginx\logs holds no access.log (only nginx.pid / its start-up messages)' (-not (Test-Path (Join-Path $run 'nginx\logs\access.log'))) ((Get-ChildItem (Join-Path $run 'nginx\logs') -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) -join ', ')
foreach ($k in $seed.Keys) {
    $exists = Test-Path -LiteralPath (Join-Path $logDir $k)
    $want = -not $seed[$k].StartsWith('deleted')
    Check "retention: $k -> $($seed[$k])" ($exists -eq $want) $(if ($exists) { 'present' } else { 'gone' })
}
[System.IO.File]::WriteAllLines((Join-Path $ev 'log-folder-after-start.txt'), [string[]](Get-ChildItem -LiteralPath $logDir | ForEach-Object { "{0,-45} {1,10} bytes" -f $_.Name, $_.Length }))

# --- 4. stop, nginx -s quit, simulator ---------------------------------------------------------------------------
$rc = Invoke-Cmd ('"' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $Package 'stop-taidaflow.ps1') + '" -Config "' + $cfgPath + '"') (Join-Path $ev 'stop.console.txt')
Check 'stop-taidaflow.ps1 -Config <run config>: exit 0' ("$rc" -eq '0') "exit $rc"
$lt = (Get-Content -LiteralPath $ll -Encoding UTF8) -join "`n"
Check "stop wrote to launcher-$d0.log (log folder from taidaflow-app.json)" ($lt -match '\[stop\]\s+log folder: .*\(taidaflow-app\.json\)' -and $lt -match '\[stop\]\s+stopped \(exit 0\)')
$exitLine = [regex]::Match($lt, 'TaidaFlowApp pid \d+ exited \(exit code (-?\d+)\)')
Check "w2-067: app exit code after WM_CLOSE (stop-taidaflow) with the simulator connected is 0" ($exitLine.Success -and $exitLine.Groups[1].Value -eq '0') $(if ($exitLine.Success) { "exit code " + $exitLine.Groups[1].Value } else { 'no exit line' })
if ($ngx.Count) {
    $ngx | ForEach-Object { $null = $_.Handle }
    $q = Start-Process -FilePath $ngxExe -ArgumentList @('-s', 'quit') -WorkingDirectory (Join-Path $run 'nginx') -WindowStyle Hidden -PassThru
    $null = $q.Handle; $null = $q.WaitForExit(20000)
    $deadline = (Get-Date).AddSeconds(20)
    while (@($ngx | Where-Object { -not $_.HasExited }).Count -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    Check "nginx -s quit (from build\w2-067-live\nginx) stopped it" (@($ngx | Where-Object { -not $_.HasExited }).Count -eq 0) "exit $($q.ExitCode)"
}
Stop-Sim
Start-Sleep -Seconds 1
$ports = @(80, 15020, 8124, 8125, 18125, 18080, 502)
$left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
Check 'nothing left listening on 80 / 502 / 15020 / 8124 / 8125 / 18125 / 18080' ($left.Count -eq 0) (($left | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" }) -join ', ')
$leftProc = @(Get-Process | Where-Object { $_.ProcessName -in 'TaidaFlowApp', 'nginx', 'Adam60xxSimulator' })
Check 'no TaidaFlowApp / nginx / Adam60xxSimulator left' ($leftProc.Count -eq 0) (($leftProc | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')
foreach ($f in @(Get-ChildItem -LiteralPath $logDir -File)) { Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $ev ('logs-' + $f.Name)) }
Note ("=== {0} check(s) failed" -f $script:fails)
Save
if ($script:fails) { exit 1 }
exit 0

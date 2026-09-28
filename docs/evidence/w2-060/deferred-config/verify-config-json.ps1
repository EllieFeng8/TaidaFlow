# w2-060 D5 (f): live check of config.json on a FIELD PACKAGE (development PC, no UI interaction).
#
# Every case runs the package with a PATH WITHOUT Qt, after scripts\safety_probe.ps1 said SAFE
# (default profile: 192.168.1.201..205:502 unreachable, no COM2, ports free - the packaged app
# connects to the plant addresses, which are not reachable here). Cases:
#   exe-config      config.json {dataDir: build\w2-060-cfg\exe-data}; TaidaFlowApp.exe started
#                   directly with the package folder as working directory (= double-click) ->
#                   log "[Config] data folder ...", files (settings.sqlite, data\, device_info.ini,
#                   exports\, TaidaFlowSettings.ini) in dataDir, none in the package folder.
#   exe-noconfig    no config.json, exe started in build\w2-060-cfg\nocfg-cwd -> "[Config] ... not
#                   found", files in that working directory (behaviour before w2-060).
#   exe-invalid     config.json {dataDir: "Q:\\no-such-drive\\TaidaFlowData"} -> warning "cannot be
#                   created", app keeps running (8124/8125 listening), files in its working directory.
#   exe-badjson     config.json with a JSON error (single backslash) -> warning "not a valid JSON
#                   object", app keeps running.
#   ps1-config      config.json {dataDir, useNginx true, nginxPort 8123, restPort 18081}; start-taidaflow.ps1
#                   and stop-taidaflow.ps1 WITHOUT parameters -> exit 0 / 0, nginx on 8123, REST on
#                   127.0.0.1:18081, http://<host>:8123/api/ 200, files in dataDir, nothing left.
#   bat-config      the same with start-taidaflow.bat / stop-taidaflow.bat (nginxPort 80, restPort 18080).
#   ps1-override    ps1-config's config.json, start-taidaflow.ps1 -DataDir <other> -UseNginx:$false ->
#                   the app uses <other> (TAIDAFLOW_DATA_DIR), no nginx; stop-taidaflow.ps1 -DataDir <other>.
#   ps1-noconfig    no config.json -> start-taidaflow.ps1: data folder <package>\runtime, no nginx;
#                   stop-taidaflow.ps1 finds it (exit 0); <package>\runtime is removed afterwards.
#   ps1-invalid / bat-invalid   dataDir on a drive that does not exist -> exit 2, nothing started.
# The package's config.json is saved first and restored at the end (also after an error).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-060\tools\verify-config-json.ps1
#            -Package dist\TaidaFlow-<...> [-Evidence docs\evidence\w2-060\40-config-json]
# Exit 0 = all checks passed, 1 = a check failed, 2 = bad parameters, 3 = probe not SAFE / ports busy.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Evidence = "docs\evidence\w2-060\40-config-json"
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
function Full([string]$p) { if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $root $p }; [System.IO.Path]::GetFullPath($p).TrimEnd('\') }
$Package = Full $Package
$Evidence = Full $Evidence
if (-not (Test-Path (Join-Path $Package 'TaidaFlowApp.exe'))) { Write-Output "no package at $Package"; exit 2 }
if (Test-Path 'Q:\') { Write-Output 'drive Q: exists - choose another invalid drive for this test'; exit 2 }
New-Item -ItemType Directory -Force $Evidence | Out-Null
$work = Full 'build\w2-060-cfg'
if (Test-Path $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force $work | Out-Null

$lines = New-Object System.Collections.Generic.List[string]
$script:fails = 0
function Note([string]$m) { $lines.Add($m); [Console]::Out.WriteLine($m) }
function Check([string]$what, [bool]$ok, [string]$detail) {
    if (-not $ok) { $script:fails++ }
    Note ("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' }))
}
function Save() { [System.IO.File]::WriteAllLines((Join-Path $Evidence 'summary.txt'), $lines.ToArray(), (New-Object System.Text.UTF8Encoding($false))) }

$pkgConfig = Join-Path $Package 'config.json'
$backup = Join-Path $Evidence 'config.json.original'
$hadConfig = Test-Path -LiteralPath $pkgConfig
if ($hadConfig) { Copy-Item -LiteralPath $pkgConfig -Destination $backup -Force }
function Restore-Config {
    if ($hadConfig) { Copy-Item -LiteralPath $backup -Destination $pkgConfig -Force } elseif (Test-Path $pkgConfig) { Remove-Item -LiteralPath $pkgConfig -Force }
}
function Set-Config($obj) {
    if ($null -eq $obj) { if (Test-Path $pkgConfig) { Remove-Item -LiteralPath $pkgConfig -Force }; Note "  config.json: (none)"; return }
    $text = if ($obj -is [string]) { $obj } else { $obj | ConvertTo-Json }
    [System.IO.File]::WriteAllText($pkgConfig, $text, (New-Object System.Text.UTF8Encoding($false)))
    Note ("  config.json: " + ($text -replace '\s+', ' '))
}

$savedEnv = @{}
foreach ($v in 'PATH', 'QT_FORCE_STDERR_LOGGING', 'TAIDAFLOW_DATA_DIR', 'TAIDAFLOW_REST_PORT', 'TAIDAFLOW_DOWNLOAD_PORT', 'TAIDAFLOW_DEVICE_PROFILE',
                'TAIDAFLOW_WEB_DIR', 'TAIDAFLOW_NGINX_PORT', 'TAIDAFLOW_NOPAUSE', 'QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH') {
    $savedEnv[$v] = [Environment]::GetEnvironmentVariable($v, 'Process')
}
function Restore-Env { foreach ($k in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($k, $savedEnv[$k], 'Process') } }
$cleanPath = @("$env:SystemRoot\System32", "$env:SystemRoot", "$env:SystemRoot\System32\Wbem", "$env:SystemRoot\System32\WindowsPowerShell\v1.0") -join ';'
function Set-CleanEnv {
    foreach ($k in $savedEnv.Keys) { if ($k -ne 'PATH') { [Environment]::SetEnvironmentVariable($k, $null, 'Process') } }
    $env:PATH = $cleanPath
}

$started = New-Object System.Collections.Generic.List[int]
trap {
    Note "EXCEPTION: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))"
    Restore-Env
    foreach ($id in $started) { $pp = Get-Process -Id $id -ErrorAction SilentlyContinue; if ($pp) { $null = $pp.CloseMainWindow(); if (-not $pp.WaitForExit(30000)) { Stop-Process -Id $id -Force } } }
    Restore-Config
    Save
    exit 1
}

$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
$lan = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' -and $_.PrefixOrigin -ne 'WellKnown' } |
         Sort-Object { if ($_.InterfaceAlias -match 'VMware|vEthernet|Virtual') { 1 } else { 0 } } | Select-Object -First 1 | ForEach-Object { $_.IPAddress })
$appPorts = @(80, 502, 8123, 8124, 8125, 18080, 18081, 18125)

function Probe([string]$case) {
    $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\safety_probe.ps1') -Reason "w2-060 config.json $case" -LogFile (Join-Path $Evidence 'safety-probe.log')
    $rc = $LASTEXITCODE
    $busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in $appPorts })
    $procs = @(Get-Process TaidaFlowApp, nginx, Adam60xxSimulator -ErrorAction SilentlyContinue)
    Note ("  probe: exit $rc ($(($o | Where-Object { $_ -match 'verdict' }) -join ' ')); listeners on $($appPorts -join '/'): $($busy.Count); TaidaFlowApp/nginx/simulator running: $($procs.Count)")
    if ($rc -ne 0 -or $busy.Count -or $procs.Count) { Note "NOT SAFE / busy - stopping here"; Restore-Env; Restore-Config; Save; exit 3 }
}
function Listening([int]$procId) { @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $procId } | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" } | Sort-Object -Unique) }
function Wait-AppPorts($p, [int]$timeoutSec = 60) {
    $t0 = Get-Date
    do { Start-Sleep -Milliseconds 500; $l = Listening $p.Id } while (-not ($l -contains '0.0.0.0:8124' -and $l -contains '0.0.0.0:8125') -and -not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $timeoutSec)
    return (Listening $p.Id)
}
function Close-App($p, [string]$case) {
    if ($p.HasExited) { Check "$case app still running before close" $false "exited with $($p.ExitCode)"; return }
    $null = $p.CloseMainWindow()
    $ok = $p.WaitForExit(60000)
    if (-not $ok) { Stop-Process -Id $p.Id -Force; $null = $p.WaitForExit(15000) }
    Check "$case app closed with WM_CLOSE" $ok "exit code $(try { $p.ExitCode } catch { '?' })"
}
function DataFiles([string]$dir) {
    @('settings.sqlite', 'data', 'device_info.ini', 'exports', 'TaidaFlowSettings.ini') | Where-Object { Test-Path (Join-Path $dir $_) }
}
function Leftovers([string]$case) {
    Start-Sleep -Seconds 1
    $procs = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue)
    $busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in $appPorts })
    Check "$case nothing left (TaidaFlowApp/nginx, listeners $($appPorts -join '/'))" ($procs.Count -eq 0 -and $busy.Count -eq 0) "processes $($procs.Count), listeners $($busy.Count)"
}
function Run-Exe([string]$case, [string]$cwd) {
    New-Item -ItemType Directory -Force $cwd | Out-Null
    $log = Join-Path $Evidence "$case.app.log"
    Set-CleanEnv
    $env:QT_FORCE_STDERR_LOGGING = '1'      # logging only: stderr -> file (no other effect)
    Note "  start $Package\TaidaFlowApp.exe (working directory $cwd, PATH without Qt)"
    $p = Start-Process -FilePath (Join-Path $Package 'TaidaFlowApp.exe') -WorkingDirectory $cwd -PassThru -RedirectStandardError $log -RedirectStandardOutput "$log.stdout"
    $null = $p.Handle
    $started.Add($p.Id)
    Restore-Env
    $l = Wait-AppPorts $p
    Note "  pid $($p.Id) listening: $($l -join ', ')"
    return $p
}
function ConfigLines([string]$case) {
    $log = Join-Path $Evidence "$case.app.log"
    Start-Sleep -Milliseconds 500
    $c = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\[Config\]|\[REST\] REST API listening' })
    $c | ForEach-Object { Note "  log | $_" }
    return ,$c
}
function Run-Script([string]$case, [string]$file, [string]$argText) {
    $out = Join-Path $Evidence "$case.console.txt"
    Set-CleanEnv
    $env:TAIDAFLOW_NOPAUSE = '1'
    $exe = if ($file -like '*.bat') { '"' + (Join-Path $Package $file) + '"' } else { '"' + "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $Package $file) + '"' }
    $cmdLine = '/c "' + $exe + $(if ($argText) { ' ' + $argText } else { '' }) + ' > "' + $out + '" 2>&1"'
    Note "  run: cmd.exe $cmdLine"
    $sp = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList $cmdLine -WindowStyle Hidden -PassThru
    $null = $sp.Handle
    $done = $sp.WaitForExit(180000)
    Restore-Env
    $rc = if ($done) { $sp.ExitCode } else { 'TIMEOUT' }
    Get-Content -LiteralPath $out -ErrorAction SilentlyContinue | Where-Object { $_ -match 'settings    :|config.json :|data folder|ERROR|REFUSED|nginx started|WARNING|exit code|TaidaFlowApp started|port .* listening|port .*NOT|stopped|exited' } |
        Select-Object -First 30 | ForEach-Object { Note "  | $_" }
    return "$rc"
}
function Http([string]$url) { (& $curl --noproxy '*' -s -o NUL -w '%{http_code}' --max-time 10 $url) }

Note "=== w2-060 config.json live check $((Get-Date).ToString('o'))"
Note "package: $Package ; work folder: $work ; LAN IPv4: $lan"
Note "original config.json: $(if ($hadConfig) { (Get-Content -Raw $pkgConfig) -replace '\s+', ' ' } else { '(none)' })"
Check 'package has config.json (shipped defaults)' $hadConfig ''

# ---- exe-config ------------------------------------------------------------------------------------
$case = 'exe-config'; Note "--- $case"
Probe $case
$data = Join-Path $work 'exe-data'
Set-Config ([ordered]@{ dataDir = $data })
$p = Run-Exe $case $Package
$c = ConfigLines $case
Check "$case log: data folder from config.json" (@($c | Where-Object { $_ -like "*data folder $data (from $pkgConfig*" }).Count -eq 1) ''
Check "$case app listens on 8124/8125" (-not $p.HasExited -and (Listening $p.Id) -contains '0.0.0.0:8125') ''
Close-App $p $case
$f = @(DataFiles $data); $pf = @(DataFiles $Package)
Check "$case files in dataDir" ($f.Count -eq 5) ($f -join ', ')
Check "$case no data files in the package folder (working directory of the start)" ($pf.Count -eq 0) ($pf -join ', ')
Leftovers $case

# ---- exe-noconfig ----------------------------------------------------------------------------------
$case = 'exe-noconfig'; Note "--- $case"
Probe $case
Set-Config $null
$cwd = Join-Path $work 'nocfg-cwd'
$p = Run-Exe $case $cwd
$c = ConfigLines $case
Check "$case log: config.json not found, data folder = working directory" ((@($c | Where-Object { $_ -like '*config.json not found*' }).Count -eq 1) -and (@($c | Where-Object { $_ -like "*data folder = current working directory $cwd*" }).Count -eq 1)) ''
Close-App $p $case
$f = @(DataFiles $cwd)
Check "$case files in the working directory (behaviour before w2-060)" ($f.Count -eq 5) ($f -join ', ')
Leftovers $case

# ---- exe-invalid -----------------------------------------------------------------------------------
$case = 'exe-invalid'; Note "--- $case"
Probe $case
Set-Config ([ordered]@{ dataDir = 'Q:\no-such-drive\TaidaFlowData' })
$cwd = Join-Path $work 'invalid-cwd'
$p = Run-Exe $case $cwd
$c = ConfigLines $case
Check "$case log: warning, data folder cannot be created, stays the working directory" (@($c | Where-Object { $_ -like '*cannot be created - data folder stays the current working directory*' }).Count -eq 1) ''
Check "$case app keeps running (8124/8125 listening, not exited)" (-not $p.HasExited -and (Listening $p.Id) -contains '0.0.0.0:8124' -and (Listening $p.Id) -contains '0.0.0.0:8125') ''
Close-App $p $case
$f = @(DataFiles $cwd)
Check "$case files in the working directory" ($f.Count -eq 5) ($f -join ', ')
Leftovers $case

# ---- exe-badjson -----------------------------------------------------------------------------------
$case = 'exe-badjson'; Note "--- $case"
Probe $case
Set-Config '{ "dataDir": "C:\TaidaFlowData" }'
$cwd = Join-Path $work 'badjson-cwd'
$p = Run-Exe $case $cwd
$c = ConfigLines $case
Check "$case log: not a valid JSON object - ignored" (@($c | Where-Object { $_ -like '*is not a valid JSON object*ignored*' }).Count -eq 1) ''
Check "$case app keeps running" (-not $p.HasExited -and (Listening $p.Id) -contains '0.0.0.0:8125') ''
Close-App $p $case
Leftovers $case

# ---- ps1-config (non-default nginx / REST ports) -----------------------------------------------------
$case = 'ps1-config'; Note "--- $case"
Probe $case
$data = Join-Path $work 'ps1-data'
Set-Config ([ordered]@{ '_comment' = 'w2-060 test'; dataDir = $data; useNginx = $true; nginxPort = 8123; restPort = 18081; unknownKey = 1 })
$rc = Run-Script $case 'start-taidaflow.ps1' ''
Check "$case start-taidaflow.ps1 (no parameters) exit 0" ($rc -eq '0') "exit $rc"
$state = try { Get-Content -Raw (Join-Path $data 'taidaflow-app.json') | ConvertFrom-Json } catch { $null }
if ($state) { $started.Add([int]$state.pid) }
$appL = if ($state) { Listening ([int]$state.pid) } else { @() }
$ngx = @(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue | Where-Object { (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).ProcessName -eq 'nginx' })
Check "$case state file in dataDir, app pid $($state.pid)" ([bool]$state) ''
Check "$case REST on 127.0.0.1:18081 (restPort) only" ($appL -contains '127.0.0.1:18081' -and -not ($appL -match ':18080$')) ($appL -join ', ')
Check "$case nginx listens on 8123 (nginxPort)" ($ngx.Count -gt 0) ''
foreach ($h in @('127.0.0.1') + $lan) {
    Check "$case http://${h}:8123/api/ -> 200 (nginx -> 18081)" ((Http "http://${h}:8123/api/") -eq '200') ''
    Check "$case http://${h}:8123/ -> 302" ((Http "http://${h}:8123/") -eq '302') ''
}
Check "$case download port = nginxPort (log)" ([bool](Select-String -Path (Join-Path $Evidence "$case.console.txt") -SimpleMatch 'TAIDAFLOW_DOWNLOAD_PORT=8123' -Quiet)) ''
Check "$case unknown key reported, _comment silent" ([bool](Select-String -Path (Join-Path $Evidence "$case.console.txt") -SimpleMatch 'unsupported key "unknownKey" ignored' -Quiet) -and -not (Select-String -Path (Join-Path $Evidence "$case.console.txt") -SimpleMatch '_comment' -Quiet)) ''
$ngConf = Join-Path $data 'nginx\conf\taidaflow.conf'
Check "$case nginx config: export folder = dataDir\exports, /api/ -> 18081" ((Test-Path $ngConf) -and (Select-String -Path $ngConf -SimpleMatch (($data -replace '\\', '/') + '/exports') -Quiet) -and (Select-String -Path $ngConf -SimpleMatch 'proxy_pass             http://127.0.0.1:18081;' -Quiet)) $ngConf
$rc = Run-Script "$case-stop" 'stop-taidaflow.ps1' ''
Check "$case stop-taidaflow.ps1 (no parameters) exit 0" ($rc -eq '0') "exit $rc"
$f = @(DataFiles $data)
Check "$case files in dataDir" ($f.Count -eq 5) ($f -join ', ')
Leftovers $case

# ---- bat-config ----------------------------------------------------------------------------------------
$case = 'bat-config'; Note "--- $case"
Probe $case
$data = Join-Path $work 'bat-data'
Set-Config ([ordered]@{ dataDir = $data; useNginx = $true; nginxPort = 80; restPort = 18080 })
$rc = Run-Script $case 'start-taidaflow.bat' ''
Check "$case start-taidaflow.bat exit 0" ($rc -eq '0') "exit $rc"
Check "$case bat message shows the data folder of config.json" ([bool](Select-String -Path (Join-Path $Evidence "$case.console.txt") -SimpleMatch "data folder (config.json dataDir): $data" -Quiet)) ''
$state = try { Get-Content -Raw (Join-Path $data 'taidaflow-app.json') | ConvertFrom-Json } catch { $null }
if ($state) { $started.Add([int]$state.pid) }
foreach ($h in @('127.0.0.1') + $lan) { Check "$case http://$h/api/settings/frequency -> 200" ((Http "http://$h/api/settings/frequency") -eq '200') '' }
$rc = Run-Script "$case-stop" 'stop-taidaflow.bat' ''
Check "$case stop-taidaflow.bat exit 0" ($rc -eq '0') "exit $rc"
$f = @(DataFiles $data)
Check "$case files in dataDir" ($f.Count -eq 5) ($f -join ', ')
Leftovers $case

# ---- ps1-override ------------------------------------------------------------------------------------
$case = 'ps1-override'; Note "--- $case"
Probe $case
Set-Config ([ordered]@{ dataDir = (Join-Path $work 'not-used'); useNginx = $true; nginxPort = 8123; restPort = 18081 })
$other = Join-Path $work 'override-data'
$rc = Run-Script $case 'start-taidaflow.ps1' ('-DataDir "' + $other + '" -UseNginx:$false')
Check "$case start with -DataDir / -UseNginx:`$false exit 0" ($rc -eq '0') "exit $rc"
$state = try { Get-Content -Raw (Join-Path $other 'taidaflow-app.json') | ConvertFrom-Json } catch { $null }
if ($state) { $started.Add([int]$state.pid) }
Check "$case no nginx started" (@(Get-Process nginx -ErrorAction SilentlyContinue).Count -eq 0) ''
$appLog = if ($state) { [string]$state.appLog } else { '' }
$rc = Run-Script "$case-stop" 'stop-taidaflow.ps1' ('-DataDir "' + $other + '"')
Check "$case stop-taidaflow.ps1 -DataDir exit 0" ($rc -eq '0') "exit $rc"
$f = @(DataFiles $other)
Check "$case app used the -DataDir folder (TAIDAFLOW_DATA_DIR), not config.json" ($f.Count -eq 5 -and -not (Test-Path (Join-Path $work 'not-used'))) ($f -join ', ')
Leftovers $case

# ---- ps1-noconfig ------------------------------------------------------------------------------------
$case = 'ps1-noconfig'; Note "--- $case"
Probe $case
Set-Config $null
$rt = Join-Path $Package 'runtime'
if (Test-Path $rt) { Note "REFUSED: $rt exists before the test"; Restore-Config; Save; exit 2 }
$rc = Run-Script $case 'start-taidaflow.ps1' ''
Check "$case start-taidaflow.ps1 exit 0" ($rc -eq '0') "exit $rc"
Check "$case data folder = <package>\runtime (default without config.json), no nginx" ((Test-Path (Join-Path $rt 'taidaflow-app.json')) -and @(Get-Process nginx -ErrorAction SilentlyContinue).Count -eq 0) ''
$state = try { Get-Content -Raw (Join-Path $rt 'taidaflow-app.json') | ConvertFrom-Json } catch { $null }
if ($state) { $started.Add([int]$state.pid) }
$rc = Run-Script "$case-stop" 'stop-taidaflow.ps1' ''
Check "$case stop-taidaflow.ps1 exit 0" ($rc -eq '0') "exit $rc"
$f = @(DataFiles $rt)
Check "$case files in <package>\runtime" ($f.Count -eq 5) ($f -join ', ')
Leftovers $case
if (Test-Path $rt) { Remove-Item -LiteralPath $rt -Recurse -Force; Note "  test data $rt removed from the package" }

# ---- ps1-invalid / bat-invalid -------------------------------------------------------------------------
foreach ($pair in @(@('ps1-invalid', 'start-taidaflow.ps1'), @('bat-invalid', 'start-taidaflow.bat'))) {
    $case = $pair[0]; Note "--- $case"
    Probe $case
    Set-Config ([ordered]@{ dataDir = 'Q:\no-such-drive\TaidaFlowData'; useNginx = $true; nginxPort = 80; restPort = 18080 })
    $rc = Run-Script $case $pair[1] ''
    Check "$case exit 2 (folder not usable), nothing started" ($rc -eq '2' -and @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue).Count -eq 0) "exit $rc"
    Leftovers $case
}

Restore-Config
Check 'package config.json restored (same bytes as before)' ((-not $hadConfig) -or ((Get-FileHash $pkgConfig).Hash -eq (Get-FileHash $backup).Hash)) ''
Note ("=== {0} check(s) failed" -f $script:fails)
Save
if ($script:fails) { exit 1 }
exit 0

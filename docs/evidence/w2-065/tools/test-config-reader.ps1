# w2-065 (copied from docs\evidence\w2-062\tools\test-config-reader.ps1 and extended): checks of
# scripts\taidaflow-config.ps1 (the config.json reader of the PowerShell scripts) against the rules of
# App/appconfig.cpp, now including the w2-064 keys nginx.exe (app default, no script fallback) and log.*,
# Resolve-TaidaFlowLogDir, Remove-TaidaFlowExpiredDatedFiles (launcher / nginx access log retention) and the
# log folder in the rendered nginx configuration. Uses build\desktop\TaidaFlowApp.exe for the defaults
# (--write-default-config only: no window, no device, no listener). Writes only below build\.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-065\tools\test-config-reader.ps1
# Exit 0 = every case passed.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
. (Join-Path $root 'scripts\taidaflow-config.ps1')
$exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$work = Join-Path $root 'build\w2-065-config-reader-test'
if (Test-Path $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force $work | Out-Null
$fails = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    if (-not $ok) { $script:fails++ }
    [Console]::Out.WriteLine(("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' })))
}
function Case([string]$name, [string]$json) {
    $d = Join-Path $work $name
    New-Item -ItemType Directory -Force $d | Out-Null
    $p = Join-Path $d 'config.json'
    if ($null -ne $json) { [System.IO.File]::WriteAllText($p, $json, (New-Object System.Text.UTF8Encoding($false))) }
    return (Get-TaidaFlowConfig -Path $p -Exe $exe)
}

# defaults = the app's own default file
$ref = Join-Path $work 'ref\config.json'
$w = Invoke-TaidaFlowWriteDefaultConfig $exe $ref
Check 'TaidaFlowApp.exe --write-default-config exit 0' ($w.rc -eq 0) $w.output
$w2 = Invoke-TaidaFlowWriteDefaultConfig $exe $ref
Check '--write-default-config on an existing file: exit 3, not overwritten' ($w2.rc -eq 3) $w2.output

New-Item -ItemType Directory -Force (Join-Path $work 'missing') | Out-Null
$c = Get-TaidaFlowConfig -Path (Join-Path $work 'missing\config.json') -Exe $exe
Check 'missing file: defaults, Exists=false, no error' (-not $c.Exists -and -not $c.Error -and $c.Values['devices.adam6256.host'] -eq '192.168.1.201' -and $c.Values['http.port'] -eq 8124 -and $c.Sources['http.port'] -eq 'default')
Check 'missing file: not created by the reader' (-not (Test-Path (Join-Path $work 'missing\config.json')))
Check 'nginx.exe default nginx\nginx.exe from the app (source default, no script fallback), resolved against the config folder' ($c.Values['nginx.exe'] -eq 'nginx\nginx.exe' -and $c.Sources['nginx.exe'] -eq 'default' -and (Resolve-TaidaFlowNginxExe $c) -eq (Join-Path $work 'missing\nginx\nginx.exe'))
Check 'log defaults from the app: dir logs, quiet on 60, full on 7 (source default), no warning' ($c.Values['log.dir'] -eq 'logs' -and $c.Values['log.quiet.enabled'] -eq $true -and $c.Values['log.quiet.keepDays'] -eq 60 -and $c.Values['log.full.enabled'] -eq $true -and $c.Values['log.full.keepDays'] -eq 7 -and $c.Sources['log.full.keepDays'] -eq 'default' -and -not @($c.Notes | Where-Object { $_ -match 'WARNING' }).Count)
Check 'log.dir default resolved against dataDir (C:\TaidaFlowData\logs)' ((Resolve-TaidaFlowLogDir $c) -eq 'C:\TaidaFlowData\logs') (Resolve-TaidaFlowLogDir $c)

$d = Join-Path $work 'create'
New-Item -ItemType Directory -Force $d | Out-Null
$c = Get-TaidaFlowConfig -Path (Join-Path $d 'config.json') -Exe $exe -Create
Check '-Create: file written by the app, identical to the reference default file' ($c.Created -and ((Get-FileHash (Join-Path $d 'config.json')).Hash -eq (Get-FileHash $ref).Hash))
Check 'created default file read back: every key from the file, no warning (log.* and nginx.exe known to the reader)' (-not @($c.Keys | Where-Object { $c.Sources[$_] -ne 'file' }).Count -and -not @($c.Notes | Where-Object { $_ -match 'WARNING|unknown' }).Count) (($c.Notes) -join ' | ')

$c = Case 'partial' '{"http": {"port": 9124}, "dataDir": "data", "foo": 1, "devices": {"adam6224": {"host": "10.1.2.3"}}}'
Check 'partial file: given values from the file, the rest defaults' ($c.Values['http.port'] -eq 9124 -and $c.Sources['http.port'] -eq 'file' -and $c.Values['devices.adam6224.host'] -eq '10.1.2.3' -and $c.Values['devices.adam6224.port'] -eq 502 -and $c.Sources['devices.adam6224.port'] -eq 'default')
Check 'relative dataDir resolved against the config folder' ($c.DataDir -eq (Join-Path $work 'partial\data'))
Check 'unknown key noted' (@($c.Notes | Where-Object { $_ -match 'unknown key "foo"' }).Count -eq 1)

$bad = '{"mirror": {"publicPort": 70000}, "rest": {"port": "18080"}, "nginx": {"port": 80.5, "enabled": "yes"}, "modbusServer": {"bind": "plant-pc"}, ' +
       '"devices": {"adam6256": {"unitId": 300, "host": "192.168.1.300"}, "ms300": {"unitId": 0, "baudRate": 1234, "dataBits": 9, "parity": "foo", "stopBits": 3, "serialPort": ""}}, "version": 2}'
$c = Case 'invalid' $bad
$expect = @{ 'mirror.publicPort' = 8125; 'rest.port' = 18080; 'nginx.port' = 80; 'nginx.enabled' = $true; 'modbusServer.bind' = '0.0.0.0'; 'devices.adam6256.unitId' = 1
             'devices.adam6256.host' = '192.168.1.201'; 'devices.ms300.unitId' = 1; 'devices.ms300.baudRate' = 9600; 'devices.ms300.dataBits' = 8
             'devices.ms300.parity' = 'none'; 'devices.ms300.stopBits' = 1; 'devices.ms300.serialPort' = 'COM2'; 'version' = 1 }
foreach ($k in $expect.Keys) {
    Check "invalid $k -> default $($expect[$k]) + warning" ($c.Values[$k] -eq $expect[$k] -and $c.Sources[$k] -eq 'default' -and @($c.Notes | Where-Object { $_ -match ('WARNING: "' + [regex]::Escape($k) + '"') }).Count -eq 1) "$($c.Values[$k])"
}
$c = Case 'normalise' '{"devices": {"adam6256": {"host": " 10.0.0.7 ", "port": 1502.0}, "ms300": {"parity": "EVEN"}}, "http": {"bind": "::1"}}'
Check 'valid values normalised (trim, 1502.0 -> 1502, EVEN -> even, IPv6)' ($c.Values['devices.adam6256.host'] -eq '10.0.0.7' -and $c.Values['devices.adam6256.port'] -eq 1502 -and $c.Values['devices.ms300.parity'] -eq 'even' -and $c.Values['http.bind'] -eq '::1' -and -not @($c.Notes | Where-Object { $_ -match 'WARNING' }).Count)
$c = Case 'group' '{"devices": 5, "mirror": {"publicPort": 8300}}'
Check 'group that is not an object: its values default + one warning' ($c.Values['devices.adam6256.host'] -eq '192.168.1.201' -and $c.Values['mirror.publicPort'] -eq 8300 -and @($c.Notes | Where-Object { $_ -match '"devices" is not an object' }).Count -eq 1)
$c = Case 'bom' ([char]0xFEFF + '{"mirror": {"publicPort": 8400}}')
Check 'UTF-8 BOM accepted' (-not $c.Error -and $c.Values['mirror.publicPort'] -eq 8400)
$c = Case 'badjson' "{`n  `"http`": {`"port`": 8124,,}`n}"
$sha = (Get-FileHash (Join-Path $work 'badjson\config.json')).Hash
Check 'broken JSON: Error set, file unchanged' ($c.Error -match 'not valid JSON' -and (Get-FileHash (Join-Path $work 'badjson\config.json')).Hash -eq $sha) $c.Error
$c = Case 'notobject' '[1, 2]'
Check 'top level not an object: Error' ($c.Error -match 'not a JSON object')
$c = Case 'empty' ''
Check 'empty file: Error' ([bool]$c.Error)
$c = Case 'nginxexe' '{"nginx": {"exe": "D:\\other\\nginx.exe"}}'
Check 'nginx.exe absolute from the file' ($c.Values['nginx.exe'] -eq 'D:\other\nginx.exe' -and $c.Sources['nginx.exe'] -eq 'file' -and (Resolve-TaidaFlowNginxExe $c) -eq 'D:\other\nginx.exe')

# effective file = complete config.json with full paths; read back = same values
$c = Case 'effective' '{"dataDir": "rel\\data", "http": {"port": 9000}}'
Set-TaidaFlowConfigValue $c 'nginx.enabled' $false
$eff = Join-Path $work 'effective-out\config.effective.json'
Write-TaidaFlowConfigFile $c $eff
$c2 = Get-TaidaFlowConfig -Path $eff -Exe $exe
$diff = @($c.Keys | Where-Object { $_ -notin 'dataDir', 'nginx.exe' -and "$($c.Values[$_])" -ne "$($c2.Values[$_])" })
Check 'effective file: complete, dataDir / nginx.exe as full paths, same values when read back' ($diff.Count -eq 0 -and $c2.DataDir -eq $c.DataDir -and $c2.Values['nginx.enabled'] -eq $false -and -not @($c2.Notes | Where-Object { $_ -match 'missing|WARNING' }).Count -and (Resolve-TaidaFlowNginxExe $c2) -eq (Resolve-TaidaFlowNginxExe $c)) ($diff -join ',')
try { Set-TaidaFlowConfigValue $c 'rest.port' 70000; Check 'override validated (70000 rejected)' $false } catch { Check 'override validated (70000 rejected)' $true $_.Exception.Message }
Check 'runtime.json body = TaidaFlowRuntime::buildRuntimeJson' ((Get-TaidaFlowRuntimeJson 8125) -ceq '{"mirrorPublicPort":8125,"version":1}')
$cd = Case 'pageport' '{"nginx": {"enabled": false, "port": 81}, "mirror": {"publicPort": 8126}, "http": {"port": 8127}}'
Check 'page port / download port: nginx off -> mirror.publicPort / http.port' ((Get-TaidaFlowPagePort $cd) -eq 8126 -and (Get-TaidaFlowDownloadPort $cd) -eq 8127)
$cd = Case 'pageport2' '{"nginx": {"enabled": true, "port": 81}}'
Check 'page port / download port: nginx on -> nginx.port' ((Get-TaidaFlowPagePort $cd) -eq 81 -and (Get-TaidaFlowDownloadPort $cd) -eq 81)
# --- w2-065: log keys (validation = App/appconfig.cpp: Text, Bool, Integer 1..2147483647) ---------------
$c = Case 'logvalid' '{"dataDir": "data", "log": {"dir": "logs\\app", "quiet": {"enabled": false, "keepDays": 365.0}, "full": {"keepDays": 1}}}'
Check 'log values from the file (365.0 -> 365, keepDays 1 accepted, enabled false)' ($c.Values['log.quiet.keepDays'] -eq 365 -and $c.Values['log.full.keepDays'] -eq 1 -and $c.Values['log.quiet.enabled'] -eq $false -and $c.Sources['log.quiet.keepDays'] -eq 'file' -and -not @($c.Notes | Where-Object { $_ -match 'WARNING' }).Count)
Check 'relative log.dir resolved against the resolved dataDir (like AppConfig::resolvedLogDir)' ((Resolve-TaidaFlowLogDir $c) -eq (Join-Path $work 'logvalid\data\logs\app')) (Resolve-TaidaFlowLogDir $c)
$c = Case 'logup' '{"dataDir": "data\\run", "log": {"dir": "..\\logs"}}'
Check 'log.dir ..\logs -> <config>\data\logs' ((Resolve-TaidaFlowLogDir $c) -eq (Join-Path $work 'logup\data\logs')) (Resolve-TaidaFlowLogDir $c)
$c = Case 'logabs' '{"log": {"dir": "E:\\plant logs\\taidaflow"}}'
Check 'absolute log.dir used as it is' ((Resolve-TaidaFlowLogDir $c) -eq 'E:\plant logs\taidaflow') (Resolve-TaidaFlowLogDir $c)
$badLog = '{"log": {"dir": "", "quiet": {"enabled": "yes", "keepDays": 0}, "full": {"enabled": 0, "keepDays": "30"}}}'
$c = Case 'loginvalid' $badLog
$expectLog = @{ 'log.dir' = 'logs'; 'log.quiet.enabled' = $true; 'log.quiet.keepDays' = 60; 'log.full.enabled' = $true; 'log.full.keepDays' = 7 }
foreach ($k in $expectLog.Keys) {
    Check "invalid $k -> default $($expectLog[$k]) + warning" ($c.Values[$k] -eq $expectLog[$k] -and $c.Sources[$k] -eq 'default' -and @($c.Notes | Where-Object { $_ -match ('WARNING: "' + [regex]::Escape($k) + '"') }).Count -eq 1) "$($c.Values[$k])"
}
foreach ($v in '-5', '1.5', '1e12', 'null', 'true') {
    $c = Case ('logkeep' + ($v -replace '[^a-z0-9]', '_')) ('{"log": {"quiet": {"keepDays": ' + $v + '}}}')
    Check "log.quiet.keepDays $v -> default 60 + warning (expected an integer 1..2147483647)" ($c.Values['log.quiet.keepDays'] -eq 60 -and @($c.Notes | Where-Object { $_ -match 'WARNING: "log\.quiet\.keepDays".*expected an integer 1\.\.2147483647' }).Count -eq 1)
}
$c = Case 'loggroup' '{"log": "yes"}'
Check 'log group that is not an object: defaults + one warning' ($c.Values['log.dir'] -eq 'logs' -and @($c.Notes | Where-Object { $_ -match '"log" is not an object' }).Count -eq 1)
$c = Case 'logmissing' '{"http": {"port": 8124}}'
Check 'config.json without log / nginx.exe (older file): defaults, only "is missing" notes, no warning' ($c.Values['log.full.keepDays'] -eq 7 -and $c.Values['nginx.exe'] -eq 'nginx\nginx.exe' -and @($c.Notes | Where-Object { $_ -match '"log\.dir" is missing' }).Count -eq 1 -and -not @($c.Notes | Where-Object { $_ -match 'WARNING' }).Count)
$c = Case 'logoverride' '{"dataDir": "d1"}'
Set-TaidaFlowConfigValue $c 'log.dir' 'E:\one-time\logs'
Set-TaidaFlowConfigValue $c 'dataDir' (Join-Path $work 'logoverride\d2')
$eff = Join-Path $work 'logoverride-out\config.effective.json'
Write-TaidaFlowConfigFile $c $eff
$c2 = Get-TaidaFlowConfig -Path $eff -Exe $exe
Check 'effective file keeps a log.dir override (absolute) and log.* values' ($c2.Values['log.dir'] -eq 'E:\one-time\logs' -and (Resolve-TaidaFlowLogDir $c2) -eq 'E:\one-time\logs' -and $c2.Values['log.quiet.keepDays'] -eq 60 -and -not @($c2.Notes | Where-Object { $_ -match 'missing|WARNING' }).Count)
$c = Case 'logrel' '{"dataDir": "d1"}'
Set-TaidaFlowConfigValue $c 'dataDir' (Join-Path $work 'logrel\d2')
Write-TaidaFlowConfigFile $c (Join-Path $work 'logrel-out\config.effective.json')
$c2 = Get-TaidaFlowConfig -Path (Join-Path $work 'logrel-out\config.effective.json') -Exe $exe
Check 'effective file with a -DataDir override: relative log.dir follows the new dataDir (same folder as the app)' ((Resolve-TaidaFlowLogDir $c2) -eq (Join-Path $work 'logrel\d2\logs') -and (Resolve-TaidaFlowLogDir $c) -eq (Resolve-TaidaFlowLogDir $c2)) (Resolve-TaidaFlowLogDir $c2)
try { Set-TaidaFlowConfigValue $c 'log.quiet.keepDays' 0; Check 'override log.quiet.keepDays 0 rejected' $false } catch { Check 'override log.quiet.keepDays 0 rejected' $true $_.Exception.Message }
Check 'Format-TaidaFlowConfig prints nginx.exe (resolved) and log.dir (resolved)' (@(Format-TaidaFlowConfig $c | Where-Object { $_ -match '^  (nginx\.exe|log\.dir) \(resolved\) = ' }).Count -eq 2)

# --- w2-065: retention of launcher-YYYY-MM-DD.log / nginx-access-YYYY-MM-DD.log ------------------------------
$ret = Join-Path $work 'retention'
New-Item -ItemType Directory -Force $ret, (Join-Path $ret 'sub') | Out-Null
$today = [datetime]'2026-09-28'
for ($i = -3; $i -le 70; $i++) {
    $day = $today.AddDays(-$i).ToString('yyyy-MM-dd')
    foreach ($p in 'launcher-', 'nginx-access-') { [System.IO.File]::WriteAllText((Join-Path $ret "$p$day.log"), 'x') }
}
$others = @('launcher.log', 'nginx-error.log', 'taidaflow-20260101-120000.log', 'taidaflow-20260101-120000.log.stdout', 'taidaflow-2020-01-01.log',
            'taidaflow-2020-01-01-full.log', 'launcher-2020-01-01.log.bak', 'launcher-2020-1-1.log', 'Launcher-2020-01-01.log', 'NGINX-ACCESS-2020-01-01.log',
            'nginx-access-2020-13-01.log', 'nginx-access-2021-02-30.log', 'nginx-access-unknown-date.log', 'launcher-2020-01-01.txt', 'my notes.txt')
foreach ($o in $others) { [System.IO.File]::WriteAllText((Join-Path $ret $o), 'x') }
[System.IO.File]::WriteAllText((Join-Path $ret 'sub\launcher-2020-01-01.log'), 'x')
New-Item -ItemType Directory -Force (Join-Path $ret 'launcher-2019-01-01.log') | Out-Null
$r = Remove-TaidaFlowExpiredDatedFiles $ret 'launcher-' '.log' 60 $today
$left = @(Get-ChildItem -LiteralPath $ret -File | Where-Object { $_.Name -cmatch '^launcher-\d{4}-\d{2}-\d{2}\.log$' -and $others -notcontains $_.Name } | ForEach-Object { $_.Name } | Sort-Object)
Check 'keepDays 60 on 2026-09-28: launcher 2026-07-31 .. 2026-10-01 kept (60 days incl. today + 3 future days), older deleted' ($r.Deleted.Count -eq 11 -and $left.Count -eq 63 -and $left[0] -eq 'launcher-2026-07-31.log' -and $left[-1] -eq 'launcher-2026-10-01.log' -and $r.Failed.Count -eq 0) "deleted $($r.Deleted.Count), kept $($left.Count), oldest $($left[0])"
$r = Remove-TaidaFlowExpiredDatedFiles $ret 'nginx-access-' '.log' 7 $today
$left = @(Get-ChildItem -LiteralPath $ret -File | Where-Object { $_.Name -cmatch '^nginx-access-\d{4}-\d{2}-\d{2}\.log$' -and $others -notcontains $_.Name } | ForEach-Object { $_.Name } | Sort-Object)
Check 'keepDays 7: nginx-access 2026-09-22 .. 2026-10-01 kept' ($left[0] -eq 'nginx-access-2026-09-22.log' -and $left.Count -eq 10) "oldest $($left[0]), kept $($left.Count)"
$r = Remove-TaidaFlowExpiredDatedFiles $ret 'nginx-access-' '.log' 1 $today
$left = @(Get-ChildItem -LiteralPath $ret -File | Where-Object { $_.Name -cmatch '^nginx-access-\d{4}-\d{2}-\d{2}\.log$' -and $others -notcontains $_.Name } | ForEach-Object { $_.Name } | Sort-Object)
Check 'keepDays 1: only today (and future dates) kept' ($left[0] -eq 'nginx-access-2026-09-28.log' -and $left.Count -eq 4) "oldest $($left[0]), kept $($left.Count)"
$stillThere = @($others | Where-Object { Test-Path -LiteralPath (Join-Path $ret $_) -PathType Leaf })
Check 'retention: every other name kept (old launcher.log, legacy taidaflow-yyyyMMdd-HHmmss.log, app logs, case variants, invalid dates, .bak, notes, sub-folder, folder named like a log)' ($stillThere.Count -eq $others.Count -and (Test-Path (Join-Path $ret 'sub\launcher-2020-01-01.log')) -and (Test-Path (Join-Path $ret 'launcher-2019-01-01.log') -PathType Container)) "$($stillThere.Count)/$($others.Count)"

# --- w2-065: the rendered nginx configuration writes its logs into the log folder -------------------------------
$tpl = Join-Path $root 'deploy\nginx\taidaflow.conf'
$conf = Get-TaidaFlowNginxConf $tpl @{ WebRoot = 'C:\inst\web'; ExportDir = 'C:\TaidaFlowData\exports'; LogDir = 'C:\TaidaFlowData\logs'; Port = 80; RestPort = 18080; MirrorPort = 18125; Prefix = 'C:\inst\nginx' }
Check 'nginx.conf: error_log "<log folder>/nginx-error.log" warn' ($conf -match '(?m)^error_log\s+"C:/TaidaFlowData/logs/nginx-error\.log" warn;')
Check 'nginx.conf: access_log "<log folder>/nginx-access-$taidaflow_log_date.log" + map $time_iso8601' ($conf -match '(?m)^\s+access_log\s+"C:/TaidaFlowData/logs/nginx-access-\$taidaflow_log_date\.log";' -and $conf -match 'map \$time_iso8601 \$taidaflow_log_date')
Check 'nginx.conf: no logs/access.log / logs/error.log any more, no @TOKEN@ left' ($conf -notmatch 'logs/access\.log' -and $conf -notmatch '(?m)^error_log\s+logs/error\.log' -and $conf -notmatch '@TAIDAFLOW_')
try { $null = Get-TaidaFlowNginxConf $tpl @{ WebRoot = 'C:\w'; ExportDir = 'C:\e'; Port = 80; RestPort = 1; MirrorPort = 2 }; Check 'Get-TaidaFlowNginxConf without LogDir refused' $false } catch { Check 'Get-TaidaFlowNginxConf without LogDir refused' $true $_.Exception.Message }
Check 'relative path helper: ../web' ((Get-TaidaFlowRelativePath 'C:\a\inst\nginx' 'C:\a\inst\web') -eq '..\web' -and (Get-TaidaFlowRelativePath 'C:\a\nginx' 'D:\web') -eq 'D:\web')

Remove-Item -LiteralPath $work -Recurse -Force
[Console]::Out.WriteLine("=== $fails check(s) failed")
if ($fails) { exit 1 }
exit 0

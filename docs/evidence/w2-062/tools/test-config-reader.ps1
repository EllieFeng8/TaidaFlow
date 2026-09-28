# w2-062: checks of scripts\taidaflow-config.ps1 (the config.json reader of the PowerShell scripts)
# against the rules of App/appconfig.cpp. Uses build\desktop\TaidaFlowApp.exe for the defaults
# (--write-default-config only: no window, no device, no listener). Writes only below build\.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-062\tools\test-config-reader.ps1
# Exit 0 = every case passed.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
. (Join-Path $root 'scripts\taidaflow-config.ps1')
$exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$work = Join-Path $root 'build\w2-062-config-reader-test'
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
Check 'nginx.exe fallback nginx\nginx.exe (script default), resolved against the config folder' ($c.Values['nginx.exe'] -eq 'nginx\nginx.exe' -and $c.Sources['nginx.exe'] -eq 'script default' -and (Resolve-TaidaFlowNginxExe $c) -eq (Join-Path $work 'missing\nginx\nginx.exe'))

$d = Join-Path $work 'create'
New-Item -ItemType Directory -Force $d | Out-Null
$c = Get-TaidaFlowConfig -Path (Join-Path $d 'config.json') -Exe $exe -Create
Check '-Create: file written by the app, identical to the reference default file' ($c.Created -and ((Get-FileHash (Join-Path $d 'config.json')).Hash -eq (Get-FileHash $ref).Hash))

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
Check 'relative path helper: ../web' ((Get-TaidaFlowRelativePath 'C:\a\inst\nginx' 'C:\a\inst\web') -eq '..\web' -and (Get-TaidaFlowRelativePath 'C:\a\nginx' 'D:\web') -eq 'D:\web')

Remove-Item -LiteralPath $work -Recurse -Force
[Console]::Out.WriteLine("=== $fails check(s) failed")
if ($fails) { exit 1 }
exit 0

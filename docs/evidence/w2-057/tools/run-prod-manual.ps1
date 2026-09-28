# w2-057 D8: follow DEPLOY_AND_STARTUP.md 1.4 (production, no scripts) once, with checks by curl / netstat / processes.
$ErrorActionPreference = 'Stop'
$r = 'D:\repo\codex\qmlTester\taidaflow'
$here = $PSScriptRoot
$ev = Join-Path $r 'docs\evidence\w2-057\20-manual-prod'
$data = Join-Path $r 'build\w2-057-manual-prod'
$pkg = Join-Path $r 'dist\TaidaFlow-20260928-c714aee'
New-Item -ItemType Directory -Force $ev | Out-Null
$lines = New-Object System.Collections.Generic.List[string]
$fails = 0
function Note([string]$m) { $lines.Add($m); [Console]::Out.WriteLine($m) }
function Check([string]$w, [bool]$ok, [string]$d) { if (-not $ok) { $script:fails++ }; Note ("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $w, $(if ($d) { " - $d" } else { '' })) }
function RunCmd([string]$name) {
    $out = Join-Path $ev "$name.out.txt"
    $p = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList ('/c ""' + (Join-Path $here "$name.cmd") + '" > "' + $out + '" 2>&1"') -WindowStyle Hidden -PassThru
    $null = $p.Handle
    if (-not $p.WaitForExit(60000)) { Note "$name TIMEOUT"; return 'TIMEOUT' }
    Get-Content $out | ForEach-Object { Note "  $name | $_" }
    return $p.ExitCode
}
function CurlHead([string]$url) {
    $ErrorActionPreference = 'Continue'
    $o = & "$env:SystemRoot\System32\curl.exe" --noproxy '*' -s -o NUL -D - --max-time 20 $url 2>&1 | ForEach-Object { "$_" }
    return @($o)
}
function Listeners { @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 80, 502, 8124, 8125, 18125 } |
                       ForEach-Object { "{0}:{1} pid {2} ({3})" -f $_.LocalAddress, $_.LocalPort, $_.OwningProcess, (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).ProcessName }) }

Note "=== DEPLOY_AND_STARTUP.md 1.4 manual production flow $((Get-Date).ToString('o'))"
$pre = @(Get-Process TaidaFlowApp, nginx, Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($pre.Count -or (Listeners).Count) { Note "busy: $((Listeners) -join '; ') $($pre | ForEach-Object { $_.ProcessName })"; exit 3 }
$probe = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r 'scripts\safety_probe.ps1') -Reason 'w2-057 D8 manual production flow (1.4) before launch' -LogFile (Join-Path $ev 'safety-probe.log')
$prc = $LASTEXITCODE
$probe | ForEach-Object { Note "  probe | $_" }
if ($prc -ne 0) { Note "probe not SAFE ($prc) - nothing started"; exit 3 }
if (Test-Path $data) { Remove-Item -LiteralPath $data -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $data 'data') | Out-Null
Copy-Item (Join-Path $r 'build\runtime-cwd\data\sensor_202609.sqlite') (Join-Path $data 'data')
Note "TEST FIXTURE: history database copied into $data\data (export check only)"

$rc = RunCmd 'prod-a-start-app'; Check '1.4 (a) cmd exit 0' ("$rc" -eq '0') "exit $rc"
$t0 = Get-Date
do { Start-Sleep -Milliseconds 500; $app = Get-Process TaidaFlowApp -ErrorAction SilentlyContinue | Select-Object -First 1
     $ok = $app -and @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $app.Id -and $_.LocalPort -in 8124, 8125 }).Count -ge 2
} while (-not $ok -and ((Get-Date) - $t0).TotalSeconds -lt 60)
Check 'TaidaFlowApp from the package runs, 8124 + 8125 listening' ([bool]$ok) $(if ($app) { "pid $($app.Id) $($app.Path)" } else { 'not running' })
if ($app) {
    $mods = @($app.Modules | ForEach-Object { $_.FileName })
    $outside = @($mods | Where-Object { (Split-Path -Leaf $_) -match '^(Qt6|vcruntime|msvcp1|concrt)' -and -not $_.StartsWith($pkg + '\', [StringComparison]::OrdinalIgnoreCase) })
    Check 'Qt / MSVC DLLs loaded from the package (PATH without Qt)' ($outside.Count -eq 0) ("{0} package modules; outside: {1}" -f @($mods | Where-Object { $_.StartsWith($pkg + '\', [StringComparison]::OrdinalIgnoreCase) }).Count, ($outside -join ', '))
}
$rc = RunCmd 'prod-b-prepare-nginx'; Check '1.4 (b) mkdir + copy exit 0' ("$rc" -eq '0') "exit $rc"
# 1.4 (b) Notepad step: the same three replacements (forward slashes).
$conf = Join-Path $data 'nginx\conf\taidaflow.conf'
$t = [IO.File]::ReadAllText($conf)
$t = $t.Replace('@TAIDAFLOW_WEB_ROOT@', ($pkg -replace '\\', '/') + '/web').Replace('@TAIDAFLOW_EXPORT_DIR@', ($data -replace '\\', '/') + '/exports').Replace('@TAIDAFLOW_NGINX_PORT@', '80')
[IO.File]::WriteAllText($conf, $t, (New-Object Text.UTF8Encoding($false)))
Note "  (Notepad step) replaced @TAIDAFLOW_WEB_ROOT@ -> $(($pkg -replace '\\', '/') + '/web'), @TAIDAFLOW_EXPORT_DIR@ -> $(($data -replace '\\', '/') + '/exports'), @TAIDAFLOW_NGINX_PORT@ -> 80"
Check 'no @TAIDAFLOW_ token left in the config' (-not ($t -match '@TAIDAFLOW_')) ''
$rc = RunCmd 'prod-c-nginx-start'
$out = Get-Content (Join-Path $ev 'prod-c-nginx-start.out.txt')
Check '1.4 (c) nginx -t: syntax is ok + test is successful' ((($out -join "`n") -match 'syntax is ok') -and (($out -join "`n") -match 'test is successful')) ''
Check '1.4 (c) curl -I http://127.0.0.1/ -> 302 Location /TaidaFlowApp.html' ((($out -join "`n") -match 'HTTP/1.1 302') -and (($out -join "`n") -match 'Location: /TaidaFlowApp.html')) ''
Note "listeners: $((Listeners) -join '; ')"
$lan = '192.168.0.125'
foreach ($u in 'http://127.0.0.1/TaidaFlowApp.html', "http://$lan/", "http://$lan/TaidaFlowApp.html", 'http://127.0.0.1:8124/TaidaFlowApp.html', "http://${lan}:8124/TaidaFlowApp.html") {
    $h = CurlHead $u; $st = ($h | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
    Check "GET $u" ("$st" -match ' (200|302)') "$st"
}
# downloadPort set by the doc's TAIDAFLOW_DOWNLOAD_PORT=80 (dev-only mirror client, same request as the web page)
$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$ErrorActionPreference = 'Continue'
$mc = & (Join-Path $r 'build\w2-057-mirror-client\mirror_export_client.exe') --url 'ws://127.0.0.1:8125/mirror' --origin 'http://127.0.0.1' --session 'w2057manual' --from-ms 1790577423000 --to-ms 1790581023000 --timeout-sec 90 2>&1 | ForEach-Object { "$_" }
$ErrorActionPreference = 'Stop'
[IO.File]::WriteAllLines((Join-Path $ev 'mirror-export.txt'), [string[]]$mc)
$res = $mc | Where-Object { $_ -match 'RESULT' } | Select-Object -Last 1
Check 'export via mirror: downloadPort=80 (TAIDAFLOW_DOWNLOAD_PORT=80 from the cmd steps)' ("$res" -match 'state=done' -and "$res" -match 'downloadPort=80\b') "$res"
if ("$res" -match ' url=(\S+)') {
    $u = "http://${lan}:80" + $Matches[1]
    $h = CurlHead $u; $st = ($h | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
    Check "download $u via nginx" ("$st" -match ' 200') "$st"
}
$rc = RunCmd 'prod-d-stop'
Start-Sleep -Seconds 3
$left = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue)
Check '1.4 (c) nginx -s quit + (d) taskkill (no /F): no TaidaFlowApp / nginx left' ($left.Count -eq 0) (($left | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')
Check 'no listener left on 80/502/8124/8125/18125' ((Listeners).Count -eq 0) ((Listeners) -join '; ')
$listing = @(Get-ChildItem -LiteralPath $data -Recurse | ForEach-Object { $_.FullName.Substring($data.Length + 1) + $(if ($_.PSIsContainer) { '\' } else { " ($($_.Length) bytes)" }) })
[IO.File]::WriteAllLines((Join-Path $ev 'datadir-listing.txt'), [string[]]$listing)
foreach ($w in 'TaidaFlowSettings.ini', 'settings.sqlite', 'data', 'exports') { Check "data folder has $w" (Test-Path (Join-Path $data $w)) '' }
Note "=== $fails check(s) failed"
[IO.File]::WriteAllLines((Join-Path $ev 'summary.txt'), $lines.ToArray())
if ($fails) { exit 1 }
exit 0

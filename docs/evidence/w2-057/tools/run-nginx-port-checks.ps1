# w2-057 D7: nginx-start / nginx-web -Port behaviour (default 80, TAIDAFLOW_NGINX_PORT, busy port, reload keeps port,
# safety probe treats 80 as information only). Only nginx (static files) is started; no app, no simulator.
$ErrorActionPreference = 'Stop'
$r = 'D:\repo\codex\qmlTester\taidaflow'
$ev = Join-Path $r 'docs\evidence\w2-057\24-nginx-port'
New-Item -ItemType Directory -Force $ev | Out-Null
$rt1 = Join-Path $r 'build\w2-057-nginx-a'; $rt2 = Join-Path $r 'build\w2-057-nginx-b'
$lines = New-Object System.Collections.Generic.List[string]; $fails = 0
function Note([string]$m) { $lines.Add($m); [Console]::Out.WriteLine($m) }
function Check([string]$w, [bool]$ok, [string]$d) { if (-not $ok) { $script:fails++ }; Note ("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $w, $(if ($d) { " - $d" } else { '' })) }
function RunPs([string]$name, [string[]]$a) {
    $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r "scripts\$name") @a
    $rc = $LASTEXITCODE
    $out | ForEach-Object { Note "  $name | $_" }
    return [pscustomobject]@{ rc = $rc; text = ($out -join "`n") }
}
Note "=== nginx port checks $((Get-Date).ToString('o'))"
if (@(Get-Process nginx -ErrorAction SilentlyContinue).Count -or @(Get-NetTCPConnection -State Listen -LocalPort 80, 8123 -ErrorAction SilentlyContinue).Count) { Note 'nginx running or 80/8123 busy - not started'; exit 3 }
foreach ($d in $rt1, $rt2) { if (Test-Path $d) { Remove-Item -LiteralPath $d -Recurse -Force } }
$x = RunPs 'nginx-start.ps1' @('-Test', '-RuntimeDir', $rt1)
Check 'nginx-start -Test (default): rendered listen 0.0.0.0:80, nginx -t ok' ($x.rc -eq 0 -and ([IO.File]::ReadAllText("$rt1\conf\taidaflow.conf") -match 'listen\s+0\.0\.0\.0:80;')) "exit $($x.rc)"
$env:TAIDAFLOW_NGINX_PORT = '8123'
$x = RunPs 'nginx-start.ps1' @('-Test', '-RuntimeDir', $rt1)
Remove-Item Env:\TAIDAFLOW_NGINX_PORT
Check 'TAIDAFLOW_NGINX_PORT=8123 + nginx-start -Test: listen 0.0.0.0:8123' ($x.rc -eq 0 -and ([IO.File]::ReadAllText("$rt1\conf\taidaflow.conf") -match 'listen\s+0\.0\.0\.0:8123;')) "exit $($x.rc)"
$x = RunPs 'nginx-start.ps1' @('-Test', '-RuntimeDir', $rt1, '-Port', '8123')
Check 'nginx-start -Test -Port 8123: listen 0.0.0.0:8123' ($x.rc -eq 0 -and ([IO.File]::ReadAllText("$rt1\conf\taidaflow.conf") -match 'listen\s+0\.0\.0\.0:8123;')) "exit $($x.rc)"
$x = RunPs 'nginx-start.ps1' @('-RuntimeDir', $rt1)
Check 'nginx-start (default port 80) -> exit 0, listening on 80' ($x.rc -eq 0 -and @(Get-NetTCPConnection -State Listen -LocalPort 80 -ErrorAction SilentlyContinue).Count -gt 0) "exit $($x.rc)"
$curl = "$env:SystemRoot\System32\curl.exe"
$h = & $curl --noproxy '*' -s -o NUL -D - --max-time 10 'http://127.0.0.1/'
Check 'curl http://127.0.0.1/ -> 302 /TaidaFlowApp.html' ((($h -join "`n") -match 'HTTP/1.1 302') -and (($h -join "`n") -match 'Location: /TaidaFlowApp.html')) (($h | Select-Object -First 1))
$x = RunPs 'nginx-start.ps1' @('-RuntimeDir', $rt2)
Check 'second nginx-start on busy port 80 -> exit 4, owner listed, nothing stopped' ($x.rc -eq 4 -and $x.text -match 'listener 0\.0\.0\.0:80 pid=\d+ \(nginx ' -and @(Get-NetTCPConnection -State Listen -LocalPort 80 -ErrorAction SilentlyContinue).Count -gt 0) "exit $($x.rc)"
$probe = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $r 'scripts\safety_probe.ps1') -Reason 'w2-057 D7: nginx on 80 is information only' -LogFile (Join-Path $ev 'safety-probe.log')
$prc = $LASTEXITCODE; $probe | ForEach-Object { Note "  probe | $_" }
Check 'safety probe with nginx on 80: "port 80 = nginx web front end ... information only", verdict SAFE' ($prc -eq 0 -and (($probe -join "`n") -match 'port 80 = nginx web front end') -and (($probe -join "`n") -match 'verdict: SAFE')) "exit $prc"
$x = RunPs 'nginx-web.ps1' @('-Action', 'reload', '-RuntimeDir', $rt1)
Check 'nginx-web reload without -Port keeps port 80' ($x.rc -eq 0 -and $x.text -match 'port 80' -and ([IO.File]::ReadAllText("$rt1\conf\taidaflow.conf") -match 'listen\s+0\.0\.0\.0:80;')) "exit $($x.rc)"
$x = RunPs 'nginx-web.ps1' @('-Action', 'status', '-RuntimeDir', $rt1)
Check 'nginx-web status -> exit 0' ($x.rc -eq 0) "exit $($x.rc)"
$x = RunPs 'nginx-stop.ps1' @('-RuntimeDir', $rt1)
Check 'nginx-stop -> exit 0, nothing on 80' ($x.rc -eq 0 -and @(Get-NetTCPConnection -State Listen -LocalPort 80 -ErrorAction SilentlyContinue).Count -eq 0 -and @(Get-Process nginx -ErrorAction SilentlyContinue).Count -eq 0) "exit $($x.rc)"
Note "=== $fails check(s) failed"
[IO.File]::WriteAllLines((Join-Path $ev 'summary.txt'), $lines.ToArray())
if ($fails) { exit 1 }; exit 0


# w2-057 D8: follow DEPLOY_AND_STARTUP.md 3.2 (test machine + simulator, cmd only) once; checks by netstat / curl / logs / processes.
$ErrorActionPreference = 'Stop'
$r = 'D:\repo\codex\qmlTester\taidaflow'
$here = $PSScriptRoot
$ev = Join-Path $r 'docs\evidence\w2-057\21-manual-test'
New-Item -ItemType Directory -Force $ev | Out-Null
$lines = New-Object System.Collections.Generic.List[string]
$fails = 0
function Note([string]$m) { $lines.Add($m); [Console]::Out.WriteLine($m) }
function Check([string]$w, [bool]$ok, [string]$d) { if (-not $ok) { $script:fails++ }; Note ("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $w, $(if ($d) { " - $d" } else { '' })) }
function RunCmd([string]$name) {
    $out = Join-Path $ev "$name.out.txt"
    $p = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList ('/c ""' + (Join-Path $here "$name.cmd") + '" > "' + $out + '" 2>&1"') -WindowStyle Hidden -PassThru
    $null = $p.Handle
    if (-not $p.WaitForExit(90000)) { Note "$name TIMEOUT"; return 'TIMEOUT' }
    Get-Content $out | ForEach-Object { Note "  $name | $_" }
    return $p.ExitCode
}
function Listeners { @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 80, 502, 8124, 8125, 18125 } |
                       ForEach-Object { "{0}:{1} pid {2} ({3})" -f $_.LocalAddress, $_.LocalPort, $_.OwningProcess, (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).ProcessName }) }
function CloseMine {
    foreach ($n in 'nginx', 'TaidaFlowApp', 'Adam60xxSimulator') {
        foreach ($p in @(Get-Process $n -ErrorAction SilentlyContinue | Where-Object { $script:mine -contains $_.Id })) {
            Note "  cleanup: closing my $n pid $($p.Id)"; $null = $p.CloseMainWindow(); if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
        }
    }
}

Note "=== DEPLOY_AND_STARTUP.md 3.2 manual test-machine flow $((Get-Date).ToString('o'))"
$pre = @(Get-Process TaidaFlowApp, nginx, Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($pre.Count -or (Listeners).Count) { Note "busy: $((Listeners) -join '; ') $($pre | ForEach-Object { $_.ProcessName })"; exit 3 }
$script:mine = @()
$rc = RunCmd 'test-0-ports'
Check '3.2 step 0: nothing listening on 80/502/8124/8125/18125' ((Get-Content (Join-Path $ev 'test-0-ports.out.txt') -Raw) -match 'findstr exit 1') ''
$rc = RunCmd 'test-1-simulator'
$sim = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue); $script:mine += @($sim | ForEach-Object { $_.Id })
$simEp = @(Get-NetTCPConnection -State Listen -LocalPort 502 -ErrorAction SilentlyContinue | Where-Object { $sim.Id -contains $_.OwningProcess } | ForEach-Object { $_.LocalAddress } | Sort-Object -Unique)
Check '3.2 step 1: simulator listening on 127.0.0.201..205:502' ($simEp.Count -eq 5) ($simEp -join ', ')
$rc = RunCmd 'test-1b-probe'
Check '3.2 step 1b: safety_probe -DeviceProfile simulator -> SAFE (exit 0)' ("$rc" -eq '0') "exit $rc"
if ("$rc" -ne '0') { Note 'probe not SAFE - app NOT started'; CloseMine; [IO.File]::WriteAllLines((Join-Path $ev 'summary.txt'), $lines.ToArray()); exit 3 }
$logFile = Join-Path $r 'build\runtime-logs\manual.log'
if (Test-Path $logFile) { Remove-Item -LiteralPath $logFile -Force }
$rc = RunCmd 'test-2-app'
$app = Get-Process TaidaFlowApp -ErrorAction SilentlyContinue | Select-Object -First 1
if ($app) { $script:mine += $app.Id }
$appL = if ($app) { @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $app.Id } | ForEach-Object { [int]$_.LocalPort } | Sort-Object -Unique) } else { @() }
Check '3.2 step 2: TaidaFlowApp (build\desktop) listening on 502/8124/8125/18125' ((@(502, 8124, 8125, 18125) | Where-Object { $appL -notcontains $_ }).Count -eq 0) ("pid $($app.Id) ports $($appL -join ',') path $($app.Path)")
$log = if (Test-Path $logFile) { Get-Content -LiteralPath $logFile } else { @() }
Copy-Item -LiteralPath $logFile -Destination (Join-Path $ev 'manual.log') -ErrorAction SilentlyContinue
Check 'manual.log: [Modbus] device profile=simulator (TAIDAFLOW_DEVICE_PROFILE from the cmd step)' (@($log | Where-Object { $_ -match '\[Modbus\] device profile=simulator' }).Count -gt 0) (($log | Where-Object { $_ -match '\[Modbus\] device (profile|ADAM)' } | Select-Object -First 3) -join ' | ')
Check 'manual.log: TAIDAFLOW_DOWNLOAD_PORT=80 - download links use port 80' (@($log | Where-Object { $_ -match 'TAIDAFLOW_DOWNLOAD_PORT=80 - download links use port 80' }).Count -gt 0) ''
Check 'manual.log: working directory build\runtime-cwd' (@($log | Where-Object { $_ -match 'runtime-cwd' }).Count -gt 0) (($log | Where-Object { $_ -match 'runtime-cwd' } | Select-Object -First 1))
$rc = RunCmd 'test-3-nginx'
$conf = Join-Path $r 'build\nginx-manual\conf\taidaflow.conf'
$t = [IO.File]::ReadAllText($conf)
$t = $t.Replace('@TAIDAFLOW_WEB_ROOT@', 'D:/repo/codex/qmlTester/taidaflow/build/desktop/web').Replace('@TAIDAFLOW_EXPORT_DIR@', 'D:/repo/codex/qmlTester/taidaflow/build/runtime-cwd/exports').Replace('@TAIDAFLOW_NGINX_PORT@', '80')
[IO.File]::WriteAllText($conf, $t, (New-Object Text.UTF8Encoding($false)))
Note '  (Notepad step) replaced the three tokens exactly as listed in 3.2 step 3'
$rc = RunCmd 'test-3b-nginx-start'
$ngx = @(Get-Process nginx -ErrorAction SilentlyContinue); $script:mine += @($ngx | ForEach-Object { $_.Id })
$o = (Get-Content (Join-Path $ev 'test-3b-nginx-start.out.txt')) -join "`n"
Check '3.2 step 3: nginx -t ok' ($o -match 'syntax is ok' -and $o -match 'test is successful') ''
Check '3.2 step 4: curl -I http://127.0.0.1/ -> 302 /TaidaFlowApp.html' ($o -match 'HTTP/1.1 302' -and $o -match 'Location: /TaidaFlowApp.html') ''
Check '3.2 step 4: curl -I page on 80 and 8124 -> 200 (two 200 responses)' ((([regex]::Matches($o, 'HTTP/1.1 200')).Count) -ge 2) ''
Note "listeners: $((Listeners) -join '; ')"
$rc = RunCmd 'test-5-close'
Start-Sleep -Seconds 2
$left = @(Get-Process TaidaFlowApp, nginx, Adam60xxSimulator -ErrorAction SilentlyContinue)
Check '3.2 step 5: nginx -s quit, taskkill app, taskkill simulator -> nothing left' ($left.Count -eq 0) (($left | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')
Check '3.2 step 5: no listener on 80/502/8124/8125/18125' ((Listeners).Count -eq 0) ((Listeners) -join '; ')
if ($left.Count) { CloseMine }
Note "=== $fails check(s) failed"
[IO.File]::WriteAllLines((Join-Path $ev 'summary.txt'), $lines.ToArray())
if ($fails) { exit 1 }
exit 0

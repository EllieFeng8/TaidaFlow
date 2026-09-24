# w2-043: LAN relay verification of the taidaflow CORE desktop build (no GUI interaction, no
# screenshots, no firewall / network setting changes).
# Adapted from taidaflow-main/build/w2-042-tools/verify-lanrelay.ps1 (w2-042, "normal"
# scenario). Differences for the core branch (which talks to real plant hardware):
#   * the app is started ONLY through scripts\run-desktop.ps1, i.e. scripts\safety_probe.ps1
#     must say SAFE first, working directory build\runtime-cwd, exe build\desktop\TaidaFlowApp.exe;
#     the probe record goes to docs\evidence\w2-043\safety-probe.log;
#   * the w2-042 "bindfail" scenario is not used: occupying 8125 now makes the probe BUSY (exit 4),
#     so the app is (correctly) not started.
# Usage: powershell -ExecutionPolicy Bypass -File docs\evidence\w2-043\tools\verify-lanrelay.ps1
# Steps: wait (every 30 s, max 30 min) while 502/8124/8125/18125 have a listener (never stops the
# owner); start app -> 0.0.0.0:8125 and 127.0.0.1:18125 listening (same app PID); WebSocket upgrade
# through 127.0.0.1:8125 and <LAN IPv4>:8125 with a non-listed Origin gets 101 and no Close frame;
# relayed legs app -> 127.0.0.1:18125 seen while connected and closed afterwards; <LAN IPv4>:18125
# refused; app closed via WM_CLOSE (exit code 0); afterwards no listener on 502/8124/8125/18125.
# Exit 0 = all checks passed; 1 = a check failed; 2 = not built; 3 = ports busy 30 min or safety
# probe not SAFE (app not started).
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$ev    = Split-Path -Parent $tools                       # docs\evidence\w2-043
$root  = (Resolve-Path (Join-Path $ev '..\..\..')).Path  # taidaflow
$exe   = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$probe = Join-Path $tools 'ws_probe.py'
$py    = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'   # real interpreter (the WindowsApps alias cannot be started by Start-Process)
$out   = Join-Path $ev 'run-lanrelay'
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out | Remove-Item -Force
$fail = @()
$app = $null
# On any script error: close OUR app instance normally (WM_CLOSE) before stopping.
trap {
    if ($app -and -not $app.HasExited) { $app.CloseMainWindow() | Out-Null; $app.WaitForExit(20000) | Out-Null }
    break
}
function Note($m) { Write-Output $m; Add-Content -Path (Join-Path $out 'summary.txt') -Value $m -Encoding utf8 }
function Listening($port) { Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -eq $port } }
function NetstatPorts { (netstat -ano | Select-String -Pattern ':8125\s|:18125\s') | ForEach-Object { $_.Line.Trim() } }

if (-not (Test-Path $exe)) { Note "not built: $exe"; exit 2 }
Note "exe = $exe"

# 1. wait until nobody else uses the ports (never stop the owner)
for ($i = 0; $i -le 60; $i++) {
    $busy = @(Listening 502) + @(Listening 8124) + @(Listening 8125) + @(Listening 18125)
    if ($busy.Count -eq 0) { break }
    if ($i -eq 60) { Note "BUSY for 30 min: $($busy | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" })"; exit 3 }
    Note "$(Get-Date -Format HH:mm:ss) ports busy ($($busy | ForEach-Object { "$($_.LocalPort)/pid $($_.OwningProcess)" })), retry in 30 s"
    Start-Sleep -Seconds 30
}
Note "$(Get-Date -Format HH:mm:ss) ports 502/8124/8125/18125 free"

$lan = (Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq 'Up' } |
        Select-Object -First 1).IPv4Address.IPAddress
Note "LAN IPv4 = $lan"
if (-not $lan) { Note 'no LAN IPv4 with a default gateway'; exit 1 }

# 2. start the app through run-desktop.ps1 (safety probe SAFE + build\runtime-cwd). Its stdout goes
#    to a file (not a pipe), so the app cannot hold this script's output pipe open.
$log = Join-Path $out 'app.stderr.log'
$runOut = Join-Path $out 'run-desktop.txt'
$runner = Start-Process powershell -PassThru -WindowStyle Hidden -RedirectStandardOutput $runOut -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $root 'scripts\run-desktop.ps1')`"",
    '-Label', 'w2-043-verify-lanrelay', '-LogFile', "`"$log`"",
    '-ProbeLog', "`"$(Join-Path $ev 'safety-probe.log')`"")
$null = $runner.Handle   # without a cached handle, ExitCode of a Start-Process -PassThru object reads as $null
$runner.WaitForExit()
$runRc = $runner.ExitCode
# If run-desktop.ps1 printed a PID, the app is running: take it over first so that every later
# exit path (including the trap) closes it.
$m = Select-String -Path $runOut -Pattern 'PID=(\d+)' | Select-Object -First 1
if ($m) {
    $appPid = [int]$m.Matches[0].Groups[1].Value
    $app = Get-Process -Id $appPid
    $null = $app.Handle   # keep a handle so ExitCode is readable later
}
Note "--- run-desktop.ps1 (exit $runRc) ---"
Get-Content $runOut | ForEach-Object { Note $_ }
if ($runRc -ne 0) {
    if ($app -and -not $app.HasExited) { $app.CloseMainWindow() | Out-Null; $app.WaitForExit(20000) | Out-Null }
    Note "RESULT NOT-RUN: run-desktop.ps1 exit $runRc (probe not SAFE or launch refused)"; exit 3
}
if (-not $app) { Note 'no PID line from run-desktop.ps1'; exit 1 }
Note "app pid $appPid ($($app.Path))"

# 3. wait for the listeners
for ($i = 0; $i -lt 150; $i++) {
    $ok = $true
    foreach ($p in 8125, 18125) { if (-not (Listening $p | Where-Object { $_.OwningProcess -eq $appPid })) { $ok = $false } }
    if ($ok) { break }
    Start-Sleep -Milliseconds 200
}
Start-Sleep -Seconds 1
Note '--- netstat (listening) ---'
NetstatPorts | ForEach-Object { Note $_ }
$l8125 = Listening 8125 | Where-Object { $_.OwningProcess -eq $appPid }
$l18125 = Listening 18125 | Where-Object { $_.OwningProcess -eq $appPid }
if (-not ($l18125 | Where-Object { $_.LocalAddress -eq '127.0.0.1' })) { $fail += 'mirror not on 127.0.0.1:18125 (app pid)' }
if (-not ($l8125 | Where-Object { $_.LocalAddress -eq '0.0.0.0' })) { $fail += 'relay not on 0.0.0.0:8125 (app pid)' }

# 4. two WebSocket upgrades through the relay, held open 6 s; non-listed Origin on purpose
$targets = @(@{ n = 'loopback'; h = '127.0.0.1' }, @{ n = 'lan'; h = $lan })
$procs = @()
foreach ($t in $targets) {
    $origin = "http://$($t.h):8123"
    $procs += Start-Process $py -ArgumentList "-B `"$probe`" connect $($t.h) 8125 $origin 6" -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out "probe-$($t.n).txt")
}
Start-Sleep -Seconds 3
Note '--- netstat (probes connected, relayed legs) ---'
NetstatPorts | Where-Object { $_ -match 'ESTABLISHED' } | ForEach-Object { Note $_ }
$legs = @(Get-NetTCPConnection -State Established -RemotePort 18125 -ErrorAction SilentlyContinue |
          Where-Object { $_.OwningProcess -eq $appPid -and $_.RemoteAddress -eq '127.0.0.1' })
Note "relayed legs app->127.0.0.1:18125 = $($legs.Count)"
if ($legs.Count -lt 2) { $fail += "expected 2 relayed legs to 18125, got $($legs.Count)" }
$procs | ForEach-Object { $_.WaitForExit(15000) | Out-Null }
foreach ($t in $targets) {
    $txt = Get-Content (Join-Path $out "probe-$($t.n).txt")
    Note "--- probe $($t.n) ($($t.h):8125) ---"; $txt | ForEach-Object { Note $_ }
    if (-not ($txt -match 'HTTP_STATUS HTTP/1.1 101')) { $fail += "$($t.n): no 101 through relay" }
    if ($txt -match 'WS_CLOSE_FRAME') { $fail += "$($t.n): server sent Close (origin rejected?)" }
}
Start-Sleep -Seconds 1
$left = @(Get-NetTCPConnection -State Established -RemotePort 18125 -ErrorAction SilentlyContinue |
          Where-Object { $_.OwningProcess -eq $appPid })
Note "relayed legs still ESTABLISHED after clients closed = $($left.Count)"
if ($left.Count -ne 0) { $fail += 'relayed legs not closed after client close' }

# 5. internal port must not be reachable on the LAN address
$r = & $py -B $probe refused $lan 18125
$refusedRc = $LASTEXITCODE
Note "--- internal port on LAN address ---"; Note $r
if ($refusedRc -ne 0) { $fail += "${lan}:18125 reachable" }

# 6. close the app normally (WM_CLOSE to its window)
$closed = $app.CloseMainWindow()
$exited = $app.WaitForExit(20000)
Note "CloseMainWindow=$closed exited=$exited exitCode=$(if ($exited) { $app.ExitCode } else { 'n/a' })"
if (-not $exited) { $fail += 'app did not exit after WM_CLOSE (left running, pid ' + $appPid + ')' }
elseif ($app.ExitCode -ne 0) { $fail += "app exit code $($app.ExitCode)" }
Start-Sleep -Seconds 1
$after = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 502, 8124, 8125, 18125 })
Note "listeners 502/8124/8125/18125 after close = $($after.Count)"
if ($after.Count -ne 0) { $fail += "listeners left after close: $($after | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)/pid $($_.OwningProcess)" })" }

Note '--- app log (relay / mirror lines) ---'
Get-Content $log -ErrorAction SilentlyContinue | Where-Object { $_ -match 'LAN relay|Mirror endpoint|WASM Mirror' } | ForEach-Object { Note $_ }
if (-not (Select-String -Path $log -Pattern 'WASM Mirror endpoint: ws://127.0.0.1:18125/mirror' -SimpleMatch -Quiet)) { $fail += 'no mirror endpoint 18125 log' }
if (-not (Select-String -Path $log -Pattern 'LAN relay listening: 0.0.0.0:8125 -> 127.0.0.1:18125' -SimpleMatch -Quiet)) { $fail += 'no relay listening log' }

if ($fail.Count -eq 0) { Note 'RESULT PASS (lanrelay)'; exit 0 }
Note "RESULT FAIL (lanrelay): $($fail -join '; ')"; exit 1

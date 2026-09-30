# w2-082 D4: live smoke of the desktop app with the embedded interface font (no UI test: no
# screenshot, no clicks; only the process, its ports, REST and its log).
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-082\tools\live-smoke.ps1
#            [-OutDir docs\evidence\w2-082\live] [-UptimeSec 75]
# Needs: build\desktop\TaidaFlowApp.exe, ..\Adam60xxSimulator\build\Adam60xxSimulator.exe.
#
# 1. Other TaidaFlowApp processes (e.g. the main-branch UI build of the PM on 127.0.0.1:8125/18125)
#    are listed and NEVER touched. Only the app started here is closed. Its ports come from
#    docs\evidence\w2-082\config.w2-082-sim.json = deploy\dev\config.simulator.json with http 8234,
#    rest 18190, mirror 18235 / 8235, nginx off and dataDir build\w2-082-run; all must be free.
# 2. Adam60xxSimulator: used when already running; otherwise started with scripts\run-simulator.ps1 and
#    closed again at the end (only the one this script started).
# 3. scripts\safety_probe.ps1 -DeviceProfile simulator with that config must be SAFE.
# 4. The app is started exactly like scripts\run-desktop.ps1 -DeviceProfile simulator does it
#    (TAIDAFLOW_CONFIG, TAIDAFLOW_DEVICE_PROFILE=simulator, QT_FORCE_STDERR_LOGGING=1, Qt bin on PATH,
#    working folder = dataDir, stderr -> log). run-desktop.ps1 itself refuses while ANY TaidaFlowApp
#    runs (someone else's included), which is why its launch lines are repeated here.
# 5. After UptimeSec the window is closed like a user (WM_CLOSE). Checks: the log has
#    "Embedded CJK font loaded: TaidaFlow Noto Sans TC" and "[UiFont] interface font: TaidaFlow Noto Sans TC
#    | Consolas -> taidaflow noto sans tc", no "[UiFont] ... is not available", no QML error, REST GET / 200,
#    sensor data saved while running, exit code 0, "[Core] shutdown complete", no listener left.
# Exit 0 = all checks passed; 1 = a check failed; 2 = refused (port in use / not built); 3 = probe not SAFE.
param(
    [string]$OutDir = "docs\evidence\w2-082\live",
    [int]$UptimeSec = 75
)
$ErrorActionPreference = 'Continue'   # native tools write to stderr (PS 5.1 would turn it into errors)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'summary.txt'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Write-Output $line; Add-Content -Encoding utf8 $summary $line }
Set-Content -Encoding utf8 $summary "=== w2-082 live smoke $(Get-Date -Format o)"
$config = Join-Path $root 'docs\evidence\w2-082\config.w2-082-sim.json'
$exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$dataDir = Join-Path $root 'build\w2-082-run'
$appLog = Join-Path $root 'build\w2-082-run-logs\desktop.log'
$ports = @(502, 8234, 8235, 18190, 18235)
$failed = 0
function Check([bool]$ok, [string]$what) { if ($ok) { Say "PASS $what" } else { Say "FAIL $what"; $script:failed++ } }

if (-not (Test-Path $exe)) { Say "not built: $exe"; exit 2 }
$others = @(Get-Process TaidaFlowApp -ErrorAction SilentlyContinue)
Say ("other TaidaFlowApp processes (not touched): " + $(if ($others.Count) { ($others | ForEach-Object { "$($_.Id) $($_.Path)" }) -join '; ' } else { 'none' }))
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
if ($busy.Count) { Say ("ports in use: " + (($busy | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" }) -join '; ') + " - refused"); exit 2 }
Say "ports free: $($ports -join ', ')"

$startedSim = $null
$sim = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($sim.Count) { Say "Adam60xxSimulator already running (pid $($sim[0].Id)) - used, not stopped" }
else {
    $p = Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'scripts\run-simulator.ps1',
            '-LogFile', 'build\w2-082-run-logs\simulator.log') -RedirectStandardOutput (Join-Path $OutDir 'run-simulator.txt') `
            -RedirectStandardError (Join-Path $OutDir 'run-simulator.err.txt') -NoNewWindow -PassThru
    $null = $p.Handle
    $p.WaitForExit()
    $text = Get-Content (Join-Path $OutDir 'run-simulator.txt') -Raw
    Say "run-simulator exit=$($p.ExitCode): $("$text".Trim())"
    $m = [regex]::Match("$text", 'SIM_PID=(\d+)')
    if ($m.Success) { $startedSim = [int]$m.Groups[1].Value } else { Say 'simulator not started'; exit 1 }
}
function Stop-OwnSimulator {
    if ($script:startedSim) {
        $s = Get-Process -Id $script:startedSim -ErrorAction SilentlyContinue
        if ($s) { $null = $s.CloseMainWindow(); if (-not $s.WaitForExit(10000)) { Stop-Process -Id $script:startedSim -Force } }
        Say "simulator pid $($script:startedSim) (started here) closed"
    }
}

& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason 'w2-082 live smoke (explicit)' `
    -DeviceProfile simulator -Config $config -Exe $exe -LogFile (Join-Path $OutDir 'safety-probe-sim.log') *> (Join-Path $OutDir 'safety-probe.console.txt')
$probe = $LASTEXITCODE
Say "safety_probe exit=$probe ($(if ($probe -eq 0) { 'SAFE' } else { 'NOT SAFE' }))"
if ($probe -ne 0) { Stop-OwnSimulator; exit 3 }

# Same launch as scripts\run-desktop.ps1 -DeviceProfile simulator (see 4. above).
New-Item -ItemType Directory -Force $dataDir, (Split-Path -Parent $appLog) | Out-Null
$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_CONFIG = $config
$env:TAIDAFLOW_DEVICE_PROFILE = 'simulator'
$app = Start-Process -FilePath $exe -WorkingDirectory $dataDir -PassThru -RedirectStandardError $appLog -RedirectStandardOutput "$appLog.stdout"
$null = $app.Handle      # keeps the exit code readable
$appPid = $app.Id
Say "app started pid=$appPid (config $config, dataDir $dataDir)"
Start-Sleep -Seconds 15
$listen = @(netstat -ano | Select-String 'LISTENING' | Select-String " $appPid$" | ForEach-Object { $_.Line.Trim() })
Say ("app listeners: " + ($listen -join ' | '))
Check ($listen.Count -ge 5) 'app listens on its 5 service ports (502, 8234, 8235, 18190, 18235)'
$rest = curl.exe -s -w '|%{http_code}' http://127.0.0.1:18190/
Check ($rest -match '"status":"ok".*\|200$') "REST GET / -> $rest"
$last1 = curl.exe -s http://127.0.0.1:18190/api/sensor/last

while (((Get-Date) - $app.StartTime).TotalSeconds -lt $UptimeSec) { Start-Sleep -Seconds 2 }
$last2 = curl.exe -s http://127.0.0.1:18190/api/sensor/last
$m1 = [regex]::Match("$last1", '"ts":(\d+)'); $m2 = [regex]::Match("$last2", '"ts":(\d+)')
$ts1 = if ($m1.Success) { [int64]$m1.Groups[1].Value } else { 0 }
$ts2 = if ($m2.Success) { [int64]$m2.Groups[1].Value } else { 0 }
Check ($ts2 -gt $ts1 -and $ts1 -gt 0) "sensor data saved while running: /api/sensor/last ts $ts1 -> $ts2"
$uptime = [int]((Get-Date) - $app.StartTime).TotalSeconds
Check ($uptime -ge 60) "uptime $uptime s >= 60 s"
$sent = $app.CloseMainWindow()
$exited = $app.WaitForExit(30000)
Say "WM_CLOSE sent=$sent exited=$exited exit_code=$(if ($exited) { $app.ExitCode } else { 'n/a' })"
Check ($exited -and $app.ExitCode -eq 0) 'app closed with exit code 0'
Start-Sleep -Seconds 1
$left = @(netstat -ano | Select-String 'LISTENING' | Select-String " $appPid$")
Check ($left.Count -eq 0) 'no listener of the app left'
$stillOthers = @($others | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
Check ($stillOthers.Count -eq $others.Count) "other TaidaFlowApp processes still running: $($stillOthers.Count) of $($others.Count)"

$enc = [System.Text.Encoding]::GetEncoding(950)
$log = [System.IO.File]::ReadAllText($appLog, $enc)
[System.IO.File]::WriteAllText((Join-Path $OutDir 'desktop.log'), $log, (New-Object System.Text.UTF8Encoding($false)))
$fontLines = @(($log -split "`r?`n") | Where-Object { $_ -match 'UiFont|Embedded CJK|CJK fallback' })
$fontLines | ForEach-Object { Say "  log: $_" }
Check ($log -match 'Embedded CJK font loaded: TaidaFlow Noto Sans TC styles: Regular,Bold') 'log: Embedded CJK font loaded: TaidaFlow Noto Sans TC (Regular, Bold)'
Check ($log -match '\[UiFont\] interface font: TaidaFlow Noto Sans TC \| Consolas -> taidaflow noto sans tc') 'log: [UiFont] interface font: TaidaFlow Noto Sans TC | Consolas -> taidaflow noto sans tc'
Check (-not ($log -match '\[UiFont\].*is not available')) 'log: no "[UiFont] ... is not available"'
Check (-not ($log -match 'CJK fallback chain')) 'log: no CJK fallback chain on the desktop (WebAssembly only)'
$qmlErr = @(($log -split "`r?`n") | Where-Object { $_ -match '(?i)qrc:/.*(error|warning)|TypeError|ReferenceError|is not a type|module .* is not installed' })
Check ($qmlErr.Count -eq 0) "log: no QML error/warning lines ($($qmlErr.Count))"
Check ($log -match '\[Core\] shutdown complete') 'log: [Core] shutdown complete'
$warn = @(($log -split "`r?`n") | Where-Object { $_ -match '(?i)warning|error|fail|critical|fatal' })
Say ("warning/error lines in the log: " + $warn.Count)
$warn | ForEach-Object { Add-Content -Encoding utf8 $summary "    $_" }

Stop-OwnSimulator
Say "=== $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })"
exit $(if ($failed) { 1 } else { 0 })

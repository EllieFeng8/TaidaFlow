# w2-085 D5 (partial live run without devices): the task's own desktop build started with
# docs\evidence\w2-085\config.w2-085-nodevice.json - the five ADAM devices at 192.0.2.201..205 (TEST-NET-1,
# RFC 5737: never reachable), all listeners on 127.0.0.1 (Modbus server 5385, http 8385, mirror 18386 / 8386,
# REST 18385), nginx off, data folder build\w2-085-run\live-nodevice (deleted first), web folder build\w2-085-wasm.
# Default device profile: scripts\safety_probe.ps1 must be SAFE. Used while the simulator run (live-run.ps1) is
# refused by the probe (another program listens on 0.0.0.0:502). No browser, no UI test.
# Checks: the monitor starts in the real exe, a settings write through the Mirror is accepted and saved
# (Ukai0107's save path), no limit alarm without a PV from the backend, app >= UptimeSec, exit code 0, no
# listener left, other processes untouched.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-085\tools\live-nodevice.ps1
# Exit 0 = all checks passed; 1 = a check failed; 2 = refused; 3 = probe not SAFE.
param([string]$OutDir = "docs\evidence\w2-085\live-nodevice", [int]$UptimeSec = 95)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'summary.txt'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss.fff') $m"; Write-Host $line; Add-Content -Encoding utf8 $summary $line }
Set-Content -Encoding utf8 $summary "=== w2-085 live run without devices $(Get-Date -Format o)"
$config = Join-Path $root 'docs\evidence\w2-085\config.w2-085-nodevice.json'
$exe = Join-Path $root 'build\w2-085-desktop\TaidaFlowApp.exe'
$webDir = Join-Path $root 'build\w2-085-wasm'
$client = Join-Path $root 'build\w2-085-run\tools\mirror_limit_client.exe'
$dataDir = Join-Path $root 'build\w2-085-run\live-nodevice'
$ports = @(5385, 8385, 8386, 18385, 18386)
$failed = 0
function Check([bool]$ok, [string]$what) { if ($ok) { Say "PASS $what" } else { Say "FAIL $what"; $script:failed++ } }

foreach ($f in @($exe, $client, (Join-Path $webDir 'TaidaFlowApp.html'))) { if (-not (Test-Path $f)) { Say "not built: $f"; exit 2 } }
$others = @(Get-Process TaidaFlowApp, nginx, Adam60xxSimulator -ErrorAction SilentlyContinue)
Say ("other TaidaFlowApp / nginx / simulator processes (not touched): " + (($others | ForEach-Object { "$($_.ProcessName) $($_.Id) $($_.Path)" }) -join '; '))
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
if ($busy.Count) { Say "ports in use - refused"; exit 2 }
if (Test-Path $dataDir) { Remove-Item -Recurse -Force $dataDir }
New-Item -ItemType Directory -Force $dataDir | Out-Null

& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason 'w2-085 live without devices' `
    -Config $config -Exe $exe -LogFile (Join-Path $OutDir 'safety-probe.log') *> (Join-Path $OutDir 'safety-probe.console.txt')
$probe = $LASTEXITCODE
Say "safety_probe (default profile) exit=$probe ($(if ($probe -eq 0) { 'SAFE' } else { 'NOT SAFE' }))"
if ($probe -ne 0) { exit 3 }

$env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_CONFIG = $config
Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue
$env:TAIDAFLOW_WEB_DIR = $webDir
$appLog = Join-Path $OutDir 'app.stderr.log'
$app = Start-Process -FilePath $exe -WorkingDirectory $dataDir -PassThru -RedirectStandardError $appLog -RedirectStandardOutput "$appLog.stdout"
$null = $app.Handle
Say "app started pid=$($app.Id)"
Start-Sleep -Seconds 10
$listen = @(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue)
Say ("app listeners: " + (($listen | Sort-Object LocalPort | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ' | '))
Check (@($listen | Where-Object { $ports -contains $_.LocalPort }).Count -eq 5 -and @($listen | Where-Object { $_.LocalAddress -ne '127.0.0.1' }).Count -eq 0) 'app listens on its 5 ports, loopback only'
$clientOut = Join-Path $OutDir 'client-nodevice.txt'
$cl = Start-Process -FilePath $client -ArgumentList @('--url', 'ws://127.0.0.1:8386/mirror', '--origin', 'http://127.0.0.1:8385', '--scenario', 'nodevice') `
        -PassThru -NoNewWindow -RedirectStandardOutput $clientOut -RedirectStandardError "$clientOut.err"
$null = $cl.Handle
$done = $cl.WaitForExit(60000)
if (-not $done) { Stop-Process -Id $cl.Id -Force }
Get-Content $clientOut -Encoding utf8 | Where-Object { $_ -match 'PASS|FAIL|WRITE|RESULT|value ' } | ForEach-Object { Say "  client: $_" }
Check ($done -and $cl.ExitCode -eq 0) "mirror client scenario nodevice exit=$(if ($done) { $cl.ExitCode } else { 'timeout' })"
while (((Get-Date) - $app.StartTime).TotalSeconds -lt $UptimeSec) { Start-Sleep -Seconds 1 }
$uptime = [int]((Get-Date) - $app.StartTime).TotalSeconds
$null = $app.CloseMainWindow()
$exited = $app.WaitForExit(30000)
Check ($uptime -ge $UptimeSec) "app ran $uptime s (>= $UptimeSec s)"
Check ($exited -and $app.ExitCode -eq 0) "app closed with exit code $(if ($exited) { $app.ExitCode } else { 'n/a' })"
Start-Sleep -Seconds 1
Check (@(Get-NetTCPConnection -State Listen -OwningProcess $app.Id -ErrorAction SilentlyContinue).Count -eq 0) 'no listener of the app left'

$full = @(Get-ChildItem (Join-Path $dataDir 'logs') -Filter '*-full.log')
$text = ($full | ForEach-Object { [System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8) }) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $OutDir 'app-full-log.txt'), $text, (New-Object System.Text.UTF8Encoding($false)))
$lines = $text -split "`r?`n"
Set-Content -Encoding utf8 (Join-Path $OutDir 'log-excerpt.txt') ($lines | Where-Object { $_ -match '\[LimitAlarm\]|\[SQL\] Alarm|\[SettingsPage\]|\[Core\] shutdown' })
function Has([string]$pattern) { return @($lines | Where-Object { $_ -match $pattern }).Count }
Check ((Has '\[LimitAlarm\] settings-page limit alarms active for 13 sensors') -eq 1) 'log: monitor active for 13 sensors in the real exe'
Check ((Has '\[LimitAlarm\] sensor settings changed: re-evaluating 0 sensor\(s\) with a value \(none yet\)') -ge 2) 'log: settings changes re-evaluated (no sensor has a value yet)'
Check ((Has "\[SettingsPage\] Saved card 'pt04'") -ge 2) 'log: the pt04 card saved twice (settings save path unchanged)'
Check ((Has '\[SQL\] Alarm inserted: sensor=(PT|TT|流量計|Filter)') -eq 0) 'log: no limit alarm and no 90 % alarm without device values'
Check ((Has '\[Core\] shutdown complete') -eq 1) 'log: [Core] shutdown complete'
Check ((Has '(?i)qrc:/.*(error|warning)|TypeError|ReferenceError') -eq 0) 'log: no QML error'
$still = @($others | Where-Object { Get-Process -Id $_.Id -ErrorAction SilentlyContinue })
Check ($still.Count -eq $others.Count) "other processes still running: $($still.Count) of $($others.Count)"
Say "=== $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })"
exit $(if ($failed) { 1 } else { 0 })

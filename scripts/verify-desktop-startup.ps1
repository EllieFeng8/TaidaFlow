# Desktop startup check (safe): scripts\run-desktop.ps1 (safety probe + build\runtime-cwd),
# wait, verify the backend and the mirror came up, close the window (WM_CLOSE) and verify
# that no listener is left behind.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\verify-desktop-startup.ps1
# Exit 0 = probe SAFE, log has the mirror endpoint and the Core's Modbus server line,
#          127.0.0.1:8125 and 0.0.0.0:502 were listening, app exited, 502/8125 free after.
# Exit 3 = safety probe not SAFE (app not started); 1 = any other check failed.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$log  = Join-Path $root 'build\runtime-logs\verify-startup.log'
$py   = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run-desktop.ps1') -Label 'verify-desktop-startup' -LogFile $log
$rc = $LASTEXITCODE
if ($rc -ne 0) { Write-Output "run-desktop.ps1 exit $rc - app not started"; exit $rc }
$p = Get-Process TaidaFlowApp -ErrorAction SilentlyContinue | Select-Object -First 1
$null = $p.Handle
Start-Sleep -Seconds 8
$l8125 = [bool](Get-NetTCPConnection -LocalAddress 127.0.0.1 -LocalPort 8125 -State Listen -ErrorAction SilentlyContinue)
$l502  = [bool](Get-NetTCPConnection -LocalPort 502 -State Listen -ErrorAction SilentlyContinue)
$text = Get-Content $log -Encoding utf8
$endpoint = [bool]($text | Select-String -SimpleMatch 'WASM Mirror endpoint: ws://127.0.0.1:8125/mirror')
$modbus   = [bool]($text | Select-String -SimpleMatch '[ModbusServer] listening on 0.0.0.0:502')
$text | Where-Object { $_ -notmatch 'request was not sent|\[MS300\]' } | Select-Object -First 25
& $py (Join-Path $PSScriptRoot 'desktop_input.py') close | Out-Null
if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force; Write-Output 'app did not exit on WM_CLOSE (killed)' }
Start-Sleep -Seconds 1
$left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 502, 8125 })
Write-Output "endpoint_line=$endpoint modbus_server_line=$modbus listening_8125=$l8125 listening_502=$l502 app_exit_code=$($p.ExitCode) listeners_left=$($left.Count)"
if ($endpoint -and $modbus -and $l8125 -and $l502 -and $left.Count -eq 0) { exit 0 } else { exit 1 }

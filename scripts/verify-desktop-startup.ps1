# Desktop startup check (safe): scripts\run-desktop.ps1 (safety probe + build\runtime-cwd),
# wait, verify the backend and the mirror came up, close the window (WM_CLOSE) and verify
# that no listener is left behind.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\verify-desktop-startup.ps1 [-ProbeLog <file>]
#   -ProbeLog: passed to run-desktop.ps1 (default docs\evidence\wasm-v4\safety-probe.log)
# Mirror layout (merged from main e4bc327 / w2-042): the desktop Mirror server itself binds
# 127.0.0.1:18125 (internal, loopback only) and App/lanrelay.h relays the public mirror port
# 0.0.0.0:8125 to it (temporary until wasm-mirror pack 1.0.2).
# Exit 0 = probe SAFE; log has 'WASM Mirror endpoint: ws://127.0.0.1:18125/mirror',
#          'LAN relay listening: 0.0.0.0:8125 -> 127.0.0.1:18125', the Core's Modbus server line
#          and the CSV download service line (w2-041); 127.0.0.1:18125 and 0.0.0.0:8125 were
#          listening, both owned by the app's PID; 0.0.0.0:502 and 0.0.0.0:8124 were listening;
#          app exited; 502/8124/8125/18125 have no listener after.
# Exit 3 = safety probe not SAFE (app not started); 1 = any other check failed.
param([string]$ProbeLog = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$log  = Join-Path $root 'build\runtime-logs\verify-startup.log'
$py   = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'

$runArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run-desktop.ps1'),
             '-Label', 'verify-desktop-startup', '-LogFile', $log)
if ($ProbeLog -ne "") { $runArgs += @('-ProbeLog', $ProbeLog) }
& powershell @runArgs
$rc = $LASTEXITCODE
if ($rc -ne 0) { Write-Output "run-desktop.ps1 exit $rc - app not started"; exit $rc }
$p = Get-Process TaidaFlowApp -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Output 'TaidaFlowApp process not found after launch'; exit 1 }
$null = $p.Handle
Start-Sleep -Seconds 8
function ListenBy($addr, $port) {
    @(Get-NetTCPConnection -LocalAddress $addr -LocalPort $port -State Listen -ErrorAction SilentlyContinue |
      Where-Object { $_.OwningProcess -eq $p.Id })
}
$l18125 = @(ListenBy '127.0.0.1' 18125).Count -gt 0  # internal mirror server (app PID)
$l8125  = @(ListenBy '0.0.0.0' 8125).Count -gt 0     # LAN relay (same app PID)
$l502   = [bool](Get-NetTCPConnection -LocalPort 502 -State Listen -ErrorAction SilentlyContinue)
$l8124  = [bool](Get-NetTCPConnection -LocalAddress 0.0.0.0 -LocalPort 8124 -State Listen -ErrorAction SilentlyContinue)
$text = Get-Content $log -Encoding utf8
$endpoint = [bool]($text | Select-String -SimpleMatch 'WASM Mirror endpoint: ws://127.0.0.1:18125/mirror')
$relay    = [bool]($text | Select-String -SimpleMatch 'LAN relay listening: 0.0.0.0:8125 -> 127.0.0.1:18125')
$modbus   = [bool]($text | Select-String -SimpleMatch '[ModbusServer] listening on 0.0.0.0:502')
$download = [bool]($text | Select-String -SimpleMatch '[ExportHTTP] download service listening on 0.0.0.0:8124')
$text | Where-Object { $_ -notmatch 'request was not sent|\[MS300\]' } | Select-Object -First 25
& $py (Join-Path $PSScriptRoot 'desktop_input.py') close | Out-Null
if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force; Write-Output 'app did not exit on WM_CLOSE (killed)' }
Start-Sleep -Seconds 1
$left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 502, 8124, 8125, 18125 })
Write-Output ("app_pid=$($p.Id) endpoint_line=$endpoint relay_line=$relay modbus_server_line=$modbus download_service_line=$download " +
              "listening_18125_app=$l18125 listening_8125_app=$l8125 listening_502=$l502 listening_8124=$l8124 " +
              "app_exit_code=$($p.ExitCode) listeners_left=$($left.Count)")
if ($endpoint -and $relay -and $modbus -and $download -and $l18125 -and $l8125 -and $l502 -and $l8124 -and $left.Count -eq 0) { exit 0 } else { exit 1 }

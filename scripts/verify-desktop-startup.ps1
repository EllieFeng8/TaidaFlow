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
#          and the HTTP service line '[Web] HTTP service listening on 0.0.0.0:8124' (w2-049:
#          AppHttpServer, web page + /exports downloads); 127.0.0.1:18125 and 0.0.0.0:8125 were
#          listening, both owned by the app's PID; 0.0.0.0:502 and 0.0.0.0:8124 were listening;
#          GET http://127.0.0.1:8124/TaidaFlowApp.html answered 200 (or 404 when the log says
#          no web page folder was found); w2-060: the REST API line '[REST] REST API listening on
#          127.0.0.1:<port>' and a listener on 127.0.0.1:<port> of the app (TAIDAFLOW_REST_PORT or
#          18080), GET http://127.0.0.1:<port>/ answered 200; app exited; 502/8124/8125/18125/<port>
#          have no listener after.
# Exit 3 = safety probe not SAFE (app not started); 1 = any other check failed.
# -DeviceProfile simulator (w2-049, TEST ONLY): passed to run-desktop.ps1 (start
#          scripts\run-simulator.ps1 first); the simulator's own 502 listeners on
#          127.0.0.201..205 are not counted as left behind.
param(
    [string]$ProbeLog = "",
    [ValidateSet('default', 'simulator')]
    [string]$DeviceProfile = "default"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$log  = Join-Path $root 'build\runtime-logs\verify-startup.log'
$py   = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'
$restPort = 18080
if ($env:TAIDAFLOW_REST_PORT) { $rp = 0; if ([int]::TryParse($env:TAIDAFLOW_REST_PORT.Trim(), [ref]$rp) -and $rp -ge 1 -and $rp -le 65535) { $restPort = $rp } }

$runArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run-desktop.ps1'),
             '-Label', 'verify-desktop-startup', '-LogFile', $log)
if ($ProbeLog -ne "") { $runArgs += @('-ProbeLog', $ProbeLog) }
if ($DeviceProfile -eq 'simulator') { $runArgs += @('-DeviceProfile', 'simulator') }
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
$lRest  = @(ListenBy '127.0.0.1' $restPort).Count -gt 0   # w2-060: REST API, loopback only (app PID)
$text = Get-Content $log -Encoding utf8
$endpoint = [bool]($text | Select-String -SimpleMatch 'WASM Mirror endpoint: ws://127.0.0.1:18125/mirror')
$relay    = [bool]($text | Select-String -SimpleMatch 'LAN relay listening: 0.0.0.0:8125 -> 127.0.0.1:18125')
$modbus   = [bool]($text | Select-String -SimpleMatch '[ModbusServer] listening on 0.0.0.0:502')
$download = [bool]($text | Select-String -SimpleMatch '[Web] HTTP service listening on 0.0.0.0:8124')
$webDir   = [bool]($text | Select-String -SimpleMatch '[Web] web page folder (')
$restLine = [bool]($text | Select-String -SimpleMatch "[REST] REST API listening on 127.0.0.1:$restPort")
$rest = & curl.exe -s -o NUL --max-time 10 -w '%{http_code}' "http://127.0.0.1:$restPort/"
$page = & curl.exe -s -o NUL --max-time 10 -w '%{http_code}' 'http://127.0.0.1:8124/TaidaFlowApp.html'
$pageOk = if ($webDir) { $page -eq '200' } else { $page -eq '404' }
$text | Where-Object { $_ -notmatch 'request was not sent|\[MS300\]' } | Select-Object -First 25
& $py (Join-Path $PSScriptRoot 'desktop_input.py') close | Out-Null
if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force; Write-Output 'app did not exit on WM_CLOSE (killed)' }
Start-Sleep -Seconds 1
$left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in @(502, 8124, 8125, 18125, $restPort) })
if ($DeviceProfile -eq 'simulator') {
    $simPids = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $left = @($left | Where-Object { -not ($_.LocalPort -eq 502 -and $simPids -contains $_.OwningProcess) })
}
Write-Output ("app_pid=$($p.Id) endpoint_line=$endpoint relay_line=$relay modbus_server_line=$modbus http_service_line=$download " +
              "web_dir_line=$webDir page_status=$page rest_line=$restLine rest_status=$rest listening_rest_app=$lRest " +
              "listening_18125_app=$l18125 listening_8125_app=$l8125 listening_502=$l502 listening_8124=$l8124 " +
              "app_exit_code=$($p.ExitCode) listeners_left=$($left.Count)")
if ($endpoint -and $relay -and $modbus -and $download -and $pageOk -and $l18125 -and $l8125 -and $l502 -and $l8124 -and
    $restLine -and $lRest -and $rest -eq '200' -and $left.Count -eq 0) { exit 0 } else { exit 1 }

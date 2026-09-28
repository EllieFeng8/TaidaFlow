# Desktop startup check (safe): scripts\run-desktop.ps1 (safety probe + config.json dataDir), wait,
# verify the backend and the mirror came up on the ports of config.json, close the window (like a user:
# WM_CLOSE through Process.CloseMainWindow, w2-062 - no Python) and verify that no listener is left.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\verify-desktop-startup.ps1
#            [-Config <config.json>] [-ProbeLog <file>] [-DeviceProfile default|simulator]
#   -Config  : passed to run-desktop.ps1 (default deploy\dev\config.dev.json); every expected address
#              and port below is read from it (scripts\taidaflow-config.ps1).
#   -ProbeLog: passed to run-desktop.ps1 (default build\runtime-logs\safety-probe[-sim].log)
# Mirror layout: the desktop Mirror server binds 127.0.0.1:<mirror.internalPort> and App/lanrelay.h
# relays <mirror.publicBind>:<mirror.publicPort> to it (temporary until wasm-mirror pack 1.0.2).
# Exit 0 = probe SAFE; the log has 'WASM Mirror endpoint: ws://127.0.0.1:<internal>/mirror',
#          'LAN relay listening: <publicBind>:<public> -> 127.0.0.1:<internal>', the Modbus server line
#          '[ModbusServer] listening on <bind>:<port>', '[Web] HTTP service listening on <bind>:<port>',
#          '[REST] REST API listening on <bind>:<port>' and '[Web] runtime.json ...'; the app listens on
#          the internal mirror, the relay, the Modbus server port, http.port and rest.port;
#          GET http://127.0.0.1:<http.port>/TaidaFlowApp.html answered 200 (or 404 when the log says no
#          web page folder was found); GET /runtime.json 200 with Cache-Control no-store and the body the
#          app wrote; GET http://127.0.0.1:<rest.port>/ answered 200; the app exited after WM_CLOSE with
#          exit code 0 and none of those ports has a listener after.
# Exit 3 = safety probe not SAFE (app not started); 1 = any other check failed.
# -DeviceProfile simulator (TEST ONLY): passed to run-desktop.ps1 (start scripts\run-simulator.ps1
#          first); the simulator's own listeners are not counted as left behind.
param(
    [string]$Config = "",
    [string]$ProbeLog = "",
    [ValidateSet('default', 'simulator')]
    [string]$DeviceProfile = "default"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
$log  = Join-Path $root 'build\runtime-logs\verify-startup.log'
if ($Config -eq "") { $Config = Join-Path $root 'deploy\dev\config.dev.json' }
elseif (-not [System.IO.Path]::IsPathRooted($Config)) { $Config = Join-Path $root $Config }
$cfg = Get-TaidaFlowConfig -Path $Config -Exe (Join-Path $root 'build\desktop\TaidaFlowApp.exe')
if ($cfg.Error) { Write-Output "config unusable: $($cfg.Error)"; exit 1 }
$v = $cfg.Values
$internal = [int]$v['mirror.internalPort']; $public = [int]$v['mirror.publicPort']; $publicBind = [string]$v['mirror.publicBind']
$serverBind = [string]$v['modbusServer.bind']; $serverPort = [int]$v['modbusServer.port']
$httpBind = [string]$v['http.bind']; $httpPort = [int]$v['http.port']
$restBind = [string]$v['rest.bind']; $restPort = [int]$v['rest.port']

$runArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run-desktop.ps1'),
             '-Label', 'verify-desktop-startup', '-LogFile', $log, '-Config', $Config)
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
$lInternal = @(ListenBy '127.0.0.1' $internal).Count -gt 0
$lPublic   = @(ListenBy $publicBind $public).Count -gt 0
$lServer   = @(ListenBy $serverBind $serverPort).Count -gt 0
$lHttp     = @(ListenBy $httpBind $httpPort).Count -gt 0
$lRest     = @(ListenBy $restBind $restPort).Count -gt 0
$text = Get-Content $log -Encoding utf8
$endpoint = [bool]($text | Select-String -SimpleMatch "WASM Mirror endpoint: ws://127.0.0.1:$internal/mirror")
$relay    = [bool]($text | Select-String -SimpleMatch "LAN relay listening: ${publicBind}:$public -> 127.0.0.1:$internal")
$modbus   = [bool]($text | Select-String -SimpleMatch "[ModbusServer] listening on ${serverBind}:$serverPort")
$download = [bool]($text | Select-String -SimpleMatch "[Web] HTTP service listening on ${httpBind}:$httpPort")
$webDir   = [bool]($text | Select-String -SimpleMatch '[Web] web page folder (')
$runtimeLine = @($text | Select-String -Pattern '\[Web\] runtime\.json (written|unchanged): (.+) = (\{.*\})') | Select-Object -First 1
$restLine = [bool]($text | Select-String -SimpleMatch "[REST] REST API listening on ${restBind}:$restPort")
$configLine = [bool]($text | Select-String -SimpleMatch '[Config] Core ')
$rest = & curl.exe -s -o NUL --max-time 10 -w '%{http_code}' "http://127.0.0.1:$restPort/"
$page = & curl.exe -s -o NUL --max-time 10 -w '%{http_code}' "http://127.0.0.1:$httpPort/TaidaFlowApp.html"
$pageOk = if ($webDir) { $page -eq '200' } else { $page -eq '404' }
$runtimeOk = -not $webDir
$runtimeDetail = 'no web folder'
if ($webDir -and $runtimeLine) {
    $expected = $runtimeLine.Matches[0].Groups[3].Value
    $rt = @(& curl.exe -s -D - --max-time 10 "http://127.0.0.1:$httpPort/runtime.json")
    $cc = ($rt | Where-Object { $_ -match '^Cache-Control:' } | Select-Object -Last 1)
    $body = ($rt | Select-Object -Last 1)
    $runtimeOk = ("$cc" -match 'no-store') -and ("$body" -ceq $expected) -and ($expected -ceq (Get-TaidaFlowRuntimeJson (Get-TaidaFlowPagePort $cfg)))
    $runtimeDetail = "$body/$("$cc".Trim())"
}
$text | Where-Object { $_ -notmatch 'request was not sent|\[MS300\]' } | Select-Object -First 25
# Close the window like a user (WM_CLOSE to the main window); the app runs its normal shutdown.
$p.Refresh()
$closeSent = $false
if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $closeSent = $p.CloseMainWindow() }
if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force; Write-Output 'app did not exit on WM_CLOSE (killed)' }
Start-Sleep -Seconds 1
$myPorts = @($serverPort, $httpPort, $public, $internal, $restPort)
$left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $myPorts -contains $_.LocalPort })
if ($DeviceProfile -eq 'simulator' -or @('adam6256', 'adam6217a', 'adam6217b', 'adam6224', 'adam6022' | Where-Object { $v["devices.$_.host"] -like '127.*' }).Count) {
    $simPids = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $left = @($left | Where-Object { -not ($simPids -contains $_.OwningProcess) })
}
Write-Output ("app_pid=$($p.Id) config_lines=$configLine endpoint_line=$endpoint relay_line=$relay modbus_server_line=$modbus http_service_line=$download " +
              "web_dir_line=$webDir page_status=$page runtime_json=$runtimeOk($runtimeDetail) rest_line=$restLine rest_status=$rest listening_rest_app=$lRest " +
              "listening_internal_${internal}_app=$lInternal listening_public_${public}_app=$lPublic listening_server_${serverPort}_app=$lServer listening_http_${httpPort}_app=$lHttp " +
              "wm_close_sent=$closeSent app_exit_code=$($p.ExitCode) listeners_left=$($left.Count)")
if ($configLine -and $endpoint -and $relay -and $modbus -and $download -and $pageOk -and $runtimeOk -and $lInternal -and $lPublic -and $lServer -and $lHttp -and
    $restLine -and $lRest -and $rest -eq '200' -and $closeSent -and $p.ExitCode -eq 0 -and $left.Count -eq 0) { exit 0 } else { exit 1 }

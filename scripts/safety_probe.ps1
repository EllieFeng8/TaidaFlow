# Safety gate (taidaflow WASM v4 spec section 2): run before EVERY desktop launch on a development PC.
#
# The core branch backend talks to real plant hardware:
#   * Modbus_Client connects to the five ADAM modules (config.json devices.adam*: host/port,
#     plant default 192.168.1.201..205:502) and writes DO/AO (pumps, valves, VFD, emergency stop).
#   * Ms300FaultReader opens the MS300 serial port (config.json devices.ms300.serialPort, COM2).
#   * Modbus_Server binds config.json modbusServer.bind:port (0.0.0.0:502).
# w2-062: every address and port below comes from the SAME config.json the app will use
# (scripts\taidaflow-config.ps1): -Config, else TAIDAFLOW_CONFIG, else deploy\dev\config.dev.json.
# Values missing in the file take the app's own defaults (TaidaFlowApp.exe --write-default-config).
#
# This script only *observes*; it never sends a Modbus frame:
#   1. Effective device addresses. For every ADAM device the address the app will really use:
#      the configured host, or with -DeviceProfile simulator (TAIDAFLOW_DEVICE_PROFILE=simulator)
#      the simulator address 127.0.0.201..205 of that device (port from config.json).
#      * a non-loopback endpoint: TCP connect (no payload, 1.5 s timeout, then close) - REACHABLE
#        = UNSAFE. With -DeviceProfile simulator the configured (plant) hosts are probed as well.
#      * a loopback endpoint (127.x = Adam60xxSimulator): it must already listen and be owned by
#        Adam60xxSimulator.exe; another owner = UNSAFE; nobody listening = SIMULATOR-NOT-READY
#        (the app would otherwise connect to its own Modbus server).
#   2. Serial ports (.NET GetPortNames + registry SERIALCOMM + PnP "(COMx)"): the configured MS300
#      port present = UNSAFE.
#   3. TCP listeners on the service ports of config.json:
#      modbusServer.port - any listener = UNSAFE (e.g. Mango's modbusserver, never stopped), except
#                          Adam60xxSimulator.exe on one of the effective loopback device endpoints;
#      http.port (web page + downloads), mirror.publicPort (LAN relay), mirror.internalPort
#      (internal mirror, 127.0.0.1) and rest.port (REST API) - any listener = BUSY (stale instance
#      or another program; the new instance could not bind it; the owner is never stopped);
#      nginx.port - information only (the optional nginx web front end, scripts\nginx-start.ps1;
#      it never talks to the plant; the app does not bind it).
# Every run appends a timestamped record to -LogFile (default build\runtime-logs\safety-probe.log,
# with -DeviceProfile simulator build\runtime-logs\safety-probe-sim.log; w2-062: no longer a file
# under docs\evidence).
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason "before run 1"
#            [-Config <config.json>] [-DeviceProfile default|simulator] [-Exe <TaidaFlowApp.exe>] [-LogFile <file>]
#   -Exe : used only for the default values of keys missing in config.json (default build\desktop\TaidaFlowApp.exe)
#
# Exit code: 0 = SAFE; 3 = UNSAFE (do not launch); 4 = BUSY (a service port is in use);
# 5 = SIMULATOR-NOT-READY; 2 = config.json unusable (not valid JSON, defaults unavailable).
param(
    [string]$Reason = "manual",
    [string]$LogFile = "",
    [string]$Config = "",
    [string]$Exe = "",
    [ValidateSet('default', 'simulator')]
    [string]$DeviceProfile = "default"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
$simulator = ($DeviceProfile -eq 'simulator')
if ($Exe -eq "") { $Exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe' }
if ($LogFile -eq "") {
    if ($simulator) { $LogFile = Join-Path $root 'build\runtime-logs\safety-probe-sim.log' }
    else { $LogFile = Join-Path $root 'build\runtime-logs\safety-probe.log' }
}
$logDir = Split-Path -Parent $LogFile
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }

$lines = New-Object System.Collections.Generic.List[string]
$stamp = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss.fffK')
$profileTag = if ($simulator) { ' profile=simulator' } else { '' }
$lines.Add("=== safety probe $stamp reason=`"$Reason`" host=$env:COMPUTERNAME$profileTag")

$configPath = Resolve-TaidaFlowConfigPath $root $Config
$cfg = Get-TaidaFlowConfig -Path $configPath -Exe $Exe
$lines.Add("  config: $configPath$(if (-not $cfg.Exists) { ' (does not exist - app defaults)' })")
foreach ($n in $cfg.Notes) { $lines.Add("  config note: $n") }
if ($cfg.Error) {
    $lines.Add("  config ERROR: $($cfg.Error)")
    $lines.Add("  verdict: CONFIG-ERROR")
    Add-Content -Path $LogFile -Value $lines -Encoding utf8
    $lines | ForEach-Object { Write-Output $_ }
    exit 2
}
$v = $cfg.Values
$deviceKeys = @('adam6256', 'adam6217a', 'adam6217b', 'adam6224', 'adam6022')
$simHosts = @{ adam6256 = '127.0.0.201'; adam6217a = '127.0.0.202'; adam6217b = '127.0.0.203'; adam6224 = '127.0.0.204'; adam6022 = '127.0.0.205' }
$serverPort = [int]$v['modbusServer.port']
$httpPort = [int]$v['http.port']; $relayPort = [int]$v['mirror.publicPort']; $mirrorPort = [int]$v['mirror.internalPort']
$restPort = [int]$v['rest.port']; $nginxPort = [int]$v['nginx.port']; $serialPort = [string]$v['devices.ms300.serialPort']

$unsafe = $false
$busy = $false
$simMissing = $false

# 0. Local IPv4 addresses (informational): the network of this host changes, so the record shows
#    which network the device probe below was made from.
try {
    $ips = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
             Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' } |
             ForEach-Object { "$($_.IPAddress)/$($_.PrefixLength)" })
    $lines.Add("  local IPv4: " + ($(if ($ips.Count) { $ips -join ', ' } else { '<none>' })))
} catch { $lines.Add("  local IPv4: <query failed: $($_.Exception.Message)>") }

function Test-Tcp([string]$ip, [int]$port) {
    $client = New-Object System.Net.Sockets.TcpClient
    $result = 'unreachable'; $detail = ''
    try {
        $task = $client.ConnectAsync($ip, $port)
        if ($task.Wait(1500)) { if ($client.Connected) { $result = 'REACHABLE' } }
        else { $detail = 'timeout 1500ms' }
    } catch {
        $e = $_.Exception
        while ($e.InnerException) { $e = $e.InnerException }
        $detail = $e.Message
    } finally { $client.Close(); $client.Dispose() }
    return @($result, $detail)
}
function Test-Loopback([string]$ip) { $a = $null; return ([System.Net.IPAddress]::TryParse($ip, [ref]$a) -and [System.Net.IPAddress]::IsLoopback($a)) }

$allListen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue)
function Get-Owner([int]$ownerPid) {
    try { $p = Get-Process -Id $ownerPid -ErrorAction Stop; return @($p.ProcessName, $p.Path) } catch { return @('?', '') }
}

# 1. Devices.
$simEndpoints = @{}     # "ip:port" of effective loopback endpoints owned by the simulator
$probed = @{}
foreach ($d in $deviceKeys) {
    $cHost = [string]$v["devices.$d.host"]; $port = [int]$v["devices.$d.port"]
    $eHost = if ($simulator) { $simHosts[$d] } else { $cHost }
    $src = "devices.$d ($($cfg.Sources["devices.$d.host"]))"
    if ($simulator -and $cHost -ne $eHost) { $src += ", simulator profile replaces $cHost" }
    $targets = @(@{ host = $eHost; what = "effective $src" })
    if ($simulator -and $cHost -ne $eHost -and -not (Test-Loopback $cHost)) { $targets += @{ host = $cHost; what = "configured devices.$d (must be unreachable)" } }
    foreach ($t in $targets) {
        $key = "$($t.host):$port"
        if ($probed.ContainsKey($key)) { continue }
        $probed[$key] = 1
        if (Test-Loopback $t.host) {
            $ls = @($allListen | Where-Object { $_.LocalPort -eq $port -and ($_.LocalAddress -eq $t.host -or $_.LocalAddress -eq '0.0.0.0' -or $_.LocalAddress -eq '::') })
            if ($ls.Count -eq 0) {
                $simMissing = $true
                $lines.Add("  device $key [$($t.what)] -> loopback, nobody listening -> SIMULATOR NOT READY (start scripts\run-simulator.ps1 first)")
            } else {
                foreach ($l in $ls) {
                    $o = Get-Owner $l.OwningProcess
                    $leaf = if ($o[1]) { Split-Path -Leaf $o[1] } else { '' }
                    if ($leaf -eq 'Adam60xxSimulator.exe' -and $l.LocalAddress -eq $t.host) {
                        $simEndpoints[$key] = 1
                        $lines.Add("  device $key [$($t.what)] -> Adam60xxSimulator ($($o[1])) pid=$($l.OwningProcess) -> accepted")
                    } else {
                        $unsafe = $true
                        $lines.Add("  device $key [$($t.what)] -> listener $($l.LocalAddress):$($l.LocalPort) pid=$($l.OwningProcess) ($($o[0]) `"$($o[1])`") is not Adam60xxSimulator.exe on $($t.host) -> UNSAFE")
                    }
                }
            }
        } else {
            $r = Test-Tcp $t.host $port
            if ($r[0] -eq 'REACHABLE') { $unsafe = $true }
            $lines.Add(("  tcp {0} [{1}] -> {2} {3}" -f $key, $t.what, $r[0], $r[1]).TrimEnd())
        }
    }
}

# 2. Serial ports.
$names = @([System.IO.Ports.SerialPort]::GetPortNames())
$reg = @()
try {
    $k = Get-ItemProperty -Path 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM' -ErrorAction Stop
    $reg = @($k.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { "$($_.Name)=$($_.Value)" })
} catch { $reg = @() }
$pnp = @()
try {
    $pnp = @(Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
             Where-Object { $_.Name -match '\(COM\d+\)' } | ForEach-Object { $_.Name })
} catch { $pnp = @("<query failed: $($_.Exception.Message)>") }
$lines.Add("  serial GetPortNames: " + ($(if ($names.Count) { $names -join ', ' } else { '<none>' })))
$lines.Add("  serial SERIALCOMM  : " + ($(if ($reg.Count) { $reg -join ', ' } else { '<none>' })))
$lines.Add("  serial PnP (COMx)  : " + ($(if ($pnp.Count) { $pnp -join ', ' } else { '<none>' })))
$allSerial = ($names + $reg + $pnp) -join ' '
if ($allSerial -match ('(?i)\b' + [regex]::Escape($serialPort) + '\b')) {
    $lines.Add("  $serialPort PRESENT (devices.ms300.serialPort, $($cfg.Sources['devices.ms300.serialPort'])) -> UNSAFE (Ms300FaultReader would open it)")
    $unsafe = $true
} else {
    $lines.Add("  $serialPort (devices.ms300.serialPort, $($cfg.Sources['devices.ms300.serialPort'])) not present")
}

# 3. Service ports.
$servicePorts = @{}; $servicePortOrder = New-Object System.Collections.Generic.List[int]
$servicePorts[$serverPort] = 'modbusServer.port (Modbus server)'; $servicePortOrder.Add($serverPort)
foreach ($pp in @(@($httpPort, 'http.port (web page + CSV downloads)'), @($relayPort, 'mirror.publicPort (LAN relay)'),
                  @($mirrorPort, 'mirror.internalPort (internal mirror)'), @($restPort, 'rest.port (REST API)'),
                  @($nginxPort, 'nginx.port (nginx web front end, information only)'))) {
    if ($servicePorts.ContainsKey([int]$pp[0])) { $servicePorts[[int]$pp[0]] += ' + ' + $pp[1] } else { $servicePorts[[int]$pp[0]] = $pp[1]; $servicePortOrder.Add([int]$pp[0]) }
}
$lines.Add("  service ports: " + (($servicePortOrder | ForEach-Object { "$_=$($servicePorts[$_])" }) -join '; '))
$listen = @($allListen | Where-Object { $servicePorts.ContainsKey([int]$_.LocalPort) })
if ($listen.Count -eq 0) { $lines.Add("  listeners on the service ports: <none>") }
foreach ($l in $listen) {
    $o = Get-Owner $l.OwningProcess
    $lp = [int]$l.LocalPort
    $lines.Add(("  listener {0}:{1} pid={2} ({3}) = {4}" -f $l.LocalAddress, $lp, $l.OwningProcess, $o[0], $servicePorts[$lp]))
    if ($lp -eq $serverPort) {
        if ($simEndpoints.ContainsKey("$($l.LocalAddress):$lp")) {
            $lines.Add("    = Adam60xxSimulator on an effective device endpoint -> accepted")
        } else {
            $unsafe = $true
            $lines.Add("    modbusServer.port $lp already in use -> conflict, do not launch (do NOT stop the owner)")
        }
    } elseif ($lp -eq $httpPort -or $lp -eq $relayPort -or $lp -eq $mirrorPort -or $lp -eq $restPort) {
        $busy = $true
        $lines.Add("    service port $lp in use -> stale desktop instance or another program, do not launch (do NOT stop the owner)")
    } elseif ($lp -eq $nginxPort) {
        $who = if ($l.OwningProcess -eq 4) { ' (pid 4 = Windows HTTP.sys, not nginx)' } else { '' }
        $lines.Add("    nginx.port $lp = web front end$who -> information only, does not block the launch")
    }
}

$verdict = if ($unsafe) { 'UNSAFE' } elseif ($busy) { 'BUSY' } elseif ($simMissing) { 'SIMULATOR-NOT-READY' } else { 'SAFE' }
$lines.Add("  verdict: $verdict")
Add-Content -Path $LogFile -Value $lines -Encoding utf8
$lines | ForEach-Object { Write-Output $_ }
if ($unsafe) { exit 3 }
if ($busy) { exit 4 }
if ($simMissing) { exit 5 }
exit 0

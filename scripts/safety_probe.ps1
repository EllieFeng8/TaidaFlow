# Safety gate (taidaflow WASM v4 spec §2): run before EVERY desktop launch.
#
# The core branch backend talks to real plant hardware:
#   * Modbus_Client connects to five ADAM modules hard-coded at 192.168.1.201..205:502
#     and writes DO/AO (pumps, valves, VFD, emergency-stop circuit).
#   * Ms300FaultReader opens COM2 (Modbus RTU to the MS300 inverter).
#   * Modbus_Server binds AnyIPv4:502.
# This script only *observes*; it never sends a Modbus frame:
#   1. TCP connect (no payload) to 192.168.1.201..205:502 with a 1.5 s timeout, then close.
#   2. Lists local serial ports (.NET GetPortNames + registry SERIALCOMM + PnP "(COMx)").
#   3. Lists TCP listeners on 502 (Core's Modbus server), 8123 (HTTP), 8124 (Core's CSV
#      download service, w2-041) and 8125 (mirror).
# Every run appends a timestamped record to docs/evidence/wasm-v4/safety-probe.log.
#
# Exit code: 0 = safe to launch; 3 = UNSAFE (a device answered, COM2 exists, or port 502
# already has a listener - e.g. Mango's modbusserver, which must NOT be stopped); the
# desktop app must not be started.  A listener on 8125 (mirror) or 8124 (export download
# service) also blocks (exit 4), because a stale desktop instance would make the result
# ambiguous (and the new instance could not bind it); a listener on 8123 is
# only reported (it is the WASM HTTP server started for the E2E test).
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\safety_probe.ps1 -Reason "before run 1"
#
# -DeviceProfile simulator (test-only; the app is then started with
# TAIDAFLOW_DEVICE_PROFILE=simulator and its five ADAM sessions go to 127.0.0.201..205):
#   * steps 1 and 2 are unchanged (192.168.1.201..205:502 must be unreachable, no COM2);
#   * a listener on port 502 is accepted ONLY when its owning process image is
#     Adam60xxSimulator.exe AND its local address is one of 127.0.0.201..205; any other 502
#     listener (another address, another process, IPv6, ...) is still UNSAFE (exit 3);
#   * all five simulator endpoints must already be listening, otherwise exit 5
#     (SIMULATOR-NOT-READY): an app started first would connect its ADAM sessions to its
#     own 0.0.0.0:502 aggregate server instead of the simulator;
#   * default log: docs/evidence/wasm-v4-sim/safety-probe.log.
# Without -DeviceProfile (or with -DeviceProfile default) the behaviour is exactly as above.
param(
    [string]$Reason = "manual",
    [string]$LogFile = "",
    [ValidateSet('default', 'simulator')]
    [string]$DeviceProfile = "default"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$simulator = ($DeviceProfile -eq 'simulator')
$simulatorHosts = @(201..205 | ForEach-Object { "127.0.0.$_" })
if ($LogFile -eq "") {
    if ($simulator) { $LogFile = Join-Path $root 'docs\evidence\wasm-v4-sim\safety-probe.log' }
    else { $LogFile = Join-Path $root 'docs\evidence\wasm-v4\safety-probe.log' }
}
$logDir = Split-Path -Parent $LogFile
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }

$lines = New-Object System.Collections.Generic.List[string]
$stamp = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss.fffK')
$profileTag = if ($simulator) { ' profile=simulator' } else { '' }
$lines.Add("=== safety probe $stamp reason=`"$Reason`" host=$env:COMPUTERNAME$profileTag")

$unsafe = $false
$busy = $false

# 0. Local IPv4 addresses (informational): the Wi-Fi segment of this host changes, so the
#    record shows which network the 192.168.1.201..205 probe below was made from.
try {
    $ips = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
             Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' } |
             ForEach-Object { "$($_.IPAddress)/$($_.PrefixLength)" })
    $lines.Add("  local IPv4: " + ($(if ($ips.Count) { $ips -join ', ' } else { '<none>' })))
} catch { $lines.Add("  local IPv4: <query failed: $($_.Exception.Message)>") }

# 1. ADAM devices: TCP connect only, 1.5 s timeout, no data written.
foreach ($i in 201..205) {
    $ip = "192.168.1.$i"
    $client = New-Object System.Net.Sockets.TcpClient
    $result = 'unreachable'
    $detail = ''
    try {
        $task = $client.ConnectAsync($ip, 502)
        if ($task.Wait(1500)) {
            if ($client.Connected) { $result = 'REACHABLE'; $unsafe = $true }
        } else {
            $detail = 'timeout 1500ms'
        }
    } catch {
        $e = $_.Exception
        while ($e.InnerException) { $e = $e.InnerException }
        $detail = $e.Message
    } finally {
        $client.Close()
        $client.Dispose()
    }
    $lines.Add(("  tcp {0}:502 -> {1} {2}" -f $ip, $result, $detail).TrimEnd())
}

# 2. Serial ports.
$names = @([System.IO.Ports.SerialPort]::GetPortNames())
$reg = @()
try {
    $key = Get-ItemProperty -Path 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM' -ErrorAction Stop
    $reg = @($key.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { "$($_.Name)=$($_.Value)" })
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
if ($allSerial -match '(?i)\bCOM2\b') {
    $lines.Add("  COM2 PRESENT -> UNSAFE (Ms300FaultReader would open it)")
    $unsafe = $true
}

# 3. Local listeners.
$listen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
            Where-Object { $_.LocalPort -in 502, 8123, 8124, 8125 })
$simListening = @()
if ($listen.Count -eq 0) {
    $lines.Add("  listeners 502/8123/8124/8125: <none>")
} else {
    foreach ($l in $listen) {
        $pname = try { (Get-Process -Id $l.OwningProcess -ErrorAction Stop).ProcessName } catch { '?' }
        $lines.Add(("  listener {0}:{1} pid={2} ({3})" -f $l.LocalAddress, $l.LocalPort, $l.OwningProcess, $pname))
        if ($l.LocalPort -eq 502 -and $simulator) {
            # Simulator profile: only Adam60xxSimulator.exe on 127.0.0.201..205 is acceptable.
            $image = try { (Get-Process -Id $l.OwningProcess -ErrorAction Stop).Path } catch { '' }
            $leaf = if ($image) { Split-Path -Leaf $image } else { '' }
            if ($leaf -eq 'Adam60xxSimulator.exe' -and $simulatorHosts -contains $l.LocalAddress) {
                $simListening += $l.LocalAddress
                $lines.Add("  port 502 on $($l.LocalAddress) = Adam60xxSimulator ($image) -> accepted (simulator profile)")
            } else {
                $unsafe = $true
                $lines.Add("  port 502 listener is not Adam60xxSimulator.exe on 127.0.0.201..205 (image=`"$image`") -> conflict, do not launch (do NOT stop the owner)")
            }
        }
        elseif ($l.LocalPort -eq 502) { $unsafe = $true; $lines.Add("  port 502 already in use -> conflict, do not launch (do NOT stop the owner)") }
        elseif ($l.LocalPort -eq 8125) { $busy = $true; $lines.Add("  port 8125 (mirror) already in use -> stale desktop instance, do not launch") }
        elseif ($l.LocalPort -eq 8124) { $busy = $true; $lines.Add("  port 8124 (CSV download service) already in use -> another instance/program, do not launch (do NOT stop the owner)") }
        else { $lines.Add("  port 8123 = WASM HTTP server (expected during E2E, informational)") }
    }
}
$simMissing = $false
if ($simulator) {
    $simListening = @($simListening | Sort-Object -Unique)
    $missing = @($simulatorHosts | Where-Object { $simListening -notcontains $_ })
    $lines.Add(("  simulator endpoints listening: {0}/5{1}" -f $simListening.Count,
                $(if ($missing.Count) { " (missing: " + ($missing -join ', ') + ")" } else { '' })))
    if ($missing.Count) {
        # Measured (w2-029, evidence 14-bind-order-core-first.txt): if the app starts first,
        # its ADAM sessions to 127.0.0.20x:502 are accepted by the app's OWN 0.0.0.0:502
        # aggregate server and stay there after the simulator starts. So all five endpoints
        # must already be listening before a simulator-profile launch.
        $simMissing = $true
        $lines.Add("  simulator not ready -> do not launch (start scripts\run-simulator.ps1 first)")
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

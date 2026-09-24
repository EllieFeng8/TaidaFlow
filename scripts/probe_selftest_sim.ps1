# Self-test of scripts\safety_probe.ps1 -DeviceProfile (default vs simulator), w2-029 D2.
# Runs the real probe in real situations and checks its exit code:
#   A  nothing on 502                         default=0  simulator=5 (SIMULATOR-NOT-READY)
#   B  foreign listener 127.0.0.201:502       default=3  simulator=3 (right address, wrong process)
#   C  Adam60xxSimulator --autostart (5/5)    default=3  simulator=0
#   D  simulator + foreign 127.0.0.1:502      simulator=3 (any other 502 listener is unsafe)
# The foreign listener is a PowerShell TcpListener started (and stopped) by this script; the
# simulator is started with scripts\run-simulator.ps1 (cwd build\sim-cwd) and closed again.
# Preconditions: nothing listening on 502/8125 and no Adam60xxSimulator/TaidaFlowApp running;
# otherwise exit 2 without touching anything. Probe records go to -LogFile.
# Exit 0 = every case matched; 1 = a mismatch; 2 = precondition failed.
param(
    [string]$LogFile = "",
    [string]$SimulatorExe = ""
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($LogFile -eq "") { $LogFile = Join-Path $root 'docs\evidence\wasm-v4-sim\safety-probe.log' }
$probe = Join-Path $PSScriptRoot 'safety_probe.ps1'

$pre = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 502, 8125 })
$procs = @(Get-Process Adam60xxSimulator, TaidaFlowApp -ErrorAction SilentlyContinue)
if ($pre.Count -or $procs.Count) {
    Write-Output "precondition failed: listeners 502/8125=$($pre.Count) processes=$(($procs | ForEach-Object { "$($_.ProcessName)#$($_.Id)" }) -join ',') - nothing touched"
    exit 2
}

$results = New-Object System.Collections.Generic.List[string]
$fail = $false
function Invoke-Probe([string]$case, [string]$prof, [int]$expect) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $probe, '-Reason', "w2-029 probe self-test $case profile=$prof", '-LogFile', $LogFile)
    if ($prof -eq 'simulator') { $a += @('-DeviceProfile', 'simulator') }
    $out = & powershell @a
    $rc = $LASTEXITCODE
    $verdict = ($out | Where-Object { $_ -match 'verdict:' } | Select-Object -Last 1).Trim()
    $ok = ($rc -eq $expect)
    if (-not $ok) { $script:fail = $true }
    $script:results.Add(("{0,-4} {1,-9} exit={2} expect={3} {4} [{5}]" -f $case, $prof, $rc, $expect, $(if ($ok) { 'OK' } else { 'MISMATCH' }), $verdict))
}
function Start-Foreign([string]$ip) {
    $cmd = "`$l=[System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse('$ip'),502); `$l.Start(); Start-Sleep -Seconds 120"
    $p = Start-Process powershell -ArgumentList '-NoProfile', '-Command', $cmd -WindowStyle Hidden -PassThru
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        if (Get-NetTCPConnection -State Listen -LocalPort 502 -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $p.Id -and $_.LocalAddress -eq $ip }) { return $p }
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw "foreign listener on ${ip}:502 did not start"
}

$foreign = $null; $sim = $null
try {
    Invoke-Probe 'A' 'default' 0
    Invoke-Probe 'A' 'simulator' 5

    $foreign = Start-Foreign '127.0.0.201'
    $results.Add("     (B: foreign listener powershell pid $($foreign.Id) on 127.0.0.201:502)")
    Invoke-Probe 'B' 'default' 3
    Invoke-Probe 'B' 'simulator' 3
    Stop-Process -Id $foreign.Id -Force; $foreign.WaitForExit(); $foreign = $null
    Start-Sleep -Milliseconds 500

    $simArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run-simulator.ps1'),
                 '-LogFile', 'build\runtime-logs\probe-selftest-simulator.log')
    if ($SimulatorExe -ne "") { $simArgs += @('-SimulatorExe', $SimulatorExe) }
    # Not '& powershell' with captured output: the simulator could inherit that pipe and the
    # capture would then only end when the simulator exits.
    $simTxt = Join-Path $root 'build\runtime-logs\probe-selftest-run-simulator.txt'
    $rs = Start-Process powershell -ArgumentList $simArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput $simTxt
    $null = $rs.Handle
    $rs.WaitForExit()
    $simOut = (Get-Content $simTxt) -join ' '
    if ($rs.ExitCode -ne 0) { throw "run-simulator.ps1 exit $($rs.ExitCode) : $simOut" }
    $simPid = [int](($simOut | Select-String 'SIM_PID=(\d+)').Matches[0].Groups[1].Value)
    $sim = Get-Process -Id $simPid
    $results.Add("     (C: $simOut)")
    Invoke-Probe 'C' 'default' 3
    Invoke-Probe 'C' 'simulator' 0

    $foreign = Start-Foreign '127.0.0.1'
    $results.Add("     (D: foreign listener powershell pid $($foreign.Id) on 127.0.0.1:502 next to the simulator)")
    Invoke-Probe 'D' 'simulator' 3
} catch {
    $fail = $true
    $results.Add("ERROR: $($_.Exception.Message)")
} finally {
    if ($foreign) { Stop-Process -Id $foreign.Id -Force -ErrorAction SilentlyContinue }
    if ($sim -and -not $sim.HasExited) {
        $null = $sim.CloseMainWindow()
        if (-not $sim.WaitForExit(8000)) { Stop-Process -Id $sim.Id -Force }
    }
}
Start-Sleep -Seconds 1
$left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -eq 502 })
$results.Add("502 listeners left after cleanup: $($left.Count)")
if ($left.Count) { $fail = $true }
$results | ForEach-Object { Write-Output $_ }
Write-Output ("probe_selftest_sim exit={0}" -f $(if ($fail) { 1 } else { 0 }))
if ($fail) { exit 1 }
exit 0

# w2-029 D1/D4: start the desktop app once per TAIDAFLOW_DEVICE_PROFILE case and check the
# Core's startup log (and, for the simulator case, the real TCP connections).
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\verify_device_profile.ps1
#            [-Cases default,bogus,simulator] [-ProbeLog <path>] [-WaitSec 10]
#   default   : variable NOT set  -> "[Modbus] device profile=default", five 192.168.1.20x:502
#               lines, no 127.0.0.20x anywhere in the log, no "Unknown TAIDAFLOW_DEVICE_PROFILE"
#   bogus     : variable = "bogus" -> the Unknown... warning + profile=default + 192.168.1.20x
#   simulator : variable = "simulator" (simulator started first with run-simulator.ps1)
#               -> profile=simulator + five 127.0.0.20x lines, no 192.168.1. in the log,
#               five ESTABLISHED app -> 127.0.0.201..205:502 owned by the simulator, and at
#               least one [Modbus][Read] with raw != 0
# Safety: before EVERY launch scripts\safety_probe.ps1 runs (default mode for default/bogus,
# -DeviceProfile simulator for simulator); any non-zero verdict -> app not started, exit 3.
# The app always runs in build\runtime-cwd; it is closed with WM_CLOSE (desktop_input.py);
# the simulator started here is closed again. Logs: build\runtime-logs\profile-<case>.log.
# Exit 0 = all cases passed; 1 = a check failed; 2 = precondition; 3 = probe not SAFE.
param(
    [string[]]$Cases = @('default', 'bogus', 'simulator'),
    [string]$ProbeLog = "",
    [int]$WaitSec = 10
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$py = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'
if ($ProbeLog -eq "") { $ProbeLog = Join-Path $root 'docs\evidence\wasm-v4-sim\safety-probe.log' }
if (-not (Test-Path $exe)) { Write-Output "not built: $exe"; exit 2 }
if (Get-Process TaidaFlowApp, Adam60xxSimulator -ErrorAction SilentlyContinue) {
    Write-Output 'TaidaFlowApp or Adam60xxSimulator already running - nothing touched'; exit 2
}
$cwd = Join-Path $root 'build\runtime-cwd'
New-Item -ItemType Directory -Force $cwd, (Join-Path $root 'build\runtime-logs') | Out-Null
$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;" + $env:PATH
$env:QT_FORCE_STDERR_LOGGING = '1'
Remove-Item Env:\TAIDAFLOW_E2E_PV_FILE -ErrorAction SilentlyContinue

$simHosts = @(201..205 | ForEach-Object { "127.0.0.$_" })
$plantHosts = @(201..205 | ForEach-Object { "192.168.1.$_" })
$fail = $false
$report = New-Object System.Collections.Generic.List[string]
function Check([string]$case, [string]$what, [bool]$ok) {
    $script:report.Add(("  [{0}] {1,-4} {2}" -f $case, $(if ($ok) { 'OK' } else { 'FAIL' }), $what))
    if (-not $ok) { $script:fail = $true }
}

foreach ($case in $Cases) {
    $sim = $null
    try {
        if ($case -eq 'simulator') {
            $simTxt = Join-Path $root 'build\runtime-logs\profile-simulator-run-simulator.txt'
            $rs = Start-Process powershell -PassThru -WindowStyle Hidden -RedirectStandardOutput $simTxt -ArgumentList @(
                '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'run-simulator.ps1'),
                '-LogFile', 'build\runtime-logs\profile-simulator-sim.log')
            $null = $rs.Handle; $rs.WaitForExit()
            $simOut = (Get-Content $simTxt) -join ' '
            if ($rs.ExitCode -ne 0) { $report.Add("  [$case] run-simulator.ps1 exit $($rs.ExitCode): $simOut"); $fail = $true; continue }
            $sim = Get-Process -Id ([int]([regex]::Match($simOut, 'SIM_PID=(\d+)').Groups[1].Value))
        }
        $probeArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'safety_probe.ps1'),
                       '-Reason', "w2-029 verify_device_profile case=$case", '-LogFile', $ProbeLog)
        if ($case -eq 'simulator') { $probeArgs += @('-DeviceProfile', 'simulator') }
        $probeOut = & powershell @probeArgs
        $probeRc = $LASTEXITCODE
        $report.Add("  [$case] probe exit=$probeRc " + (($probeOut | Where-Object { $_ -match 'verdict' }) -join ''))
        if ($probeRc -ne 0) { Write-Output ($report -join "`n"); Write-Output 'probe not SAFE - app NOT started'; exit 3 }

        switch ($case) {
            'default'   { Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue }
            'bogus'     { $env:TAIDAFLOW_DEVICE_PROFILE = 'bogus' }
            'simulator' { $env:TAIDAFLOW_DEVICE_PROFILE = 'simulator' }
        }
        $log = Join-Path $root "build\runtime-logs\profile-$case.log"
        $p = Start-Process -FilePath $exe -WorkingDirectory $cwd -PassThru -RedirectStandardError $log -RedirectStandardOutput "$log.stdout"
        $null = $p.Handle
        Start-Sleep -Seconds $WaitSec
        $conns = @(Get-NetTCPConnection -OwningProcess $p.Id -ErrorAction SilentlyContinue |
                   Where-Object { $_.RemotePort -eq 502 -and $_.State -eq 'Established' })
        $simOwned = @()
        if ($sim) {
            $simOwned = @(Get-NetTCPConnection -OwningProcess $sim.Id -State Established -ErrorAction SilentlyContinue |
                          Where-Object { $_.LocalPort -eq 502 } | ForEach-Object { $_.LocalAddress } | Sort-Object -Unique)
        }
        & $py (Join-Path $PSScriptRoot 'desktop_input.py') close | Out-Null
        if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force; $report.Add("  [$case] app did not exit on WM_CLOSE (killed)"); $fail = $true }
        Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue

        $t = @(Get-Content $log -Encoding utf8)
        $prof = @($t | Where-Object { $_ -match '^\[Modbus\] device profile=' })
        $addr = @($t | Where-Object { $_ -match '^\[Modbus\] device .+ -> ' })
        $report.Add("  [$case] log $log ($($t.Count) lines):")
        ($prof + $addr) | ForEach-Object { $report.Add("      $_") }
        $warn = @($t | Where-Object { $_ -match 'Unknown TAIDAFLOW_DEVICE_PROFILE' })
        $warn | ForEach-Object { $report.Add("      $_") }
        $expectHosts = if ($case -eq 'simulator') { $simHosts } else { $plantHosts }
        $expectProf = if ($case -eq 'simulator') { 'simulator' } else { 'default' }
        Check $case "profile line = $expectProf" ($prof.Count -eq 1 -and $prof[0] -eq "[Modbus] device profile=$expectProf")
        Check $case "five address lines = $($expectHosts[0])..205:502 unit=1" (
            $addr.Count -eq 5 -and @($expectHosts | Where-Object { $h = $_; -not ($addr | Where-Object { $_ -like "* -> ${h}:502 unit=1" }) }).Count -eq 0)
        Check $case "Unknown-profile warning present = $($case -eq 'bogus')" (($warn.Count -gt 0) -eq ($case -eq 'bogus'))
        if ($case -eq 'simulator') {
            Check $case 'no 192.168.1. anywhere in the log' (@($t | Where-Object { $_ -match '192\.168\.1\.' }).Count -eq 0)
            $remotes = @($conns | ForEach-Object { $_.RemoteAddress } | Sort-Object -Unique)
            $report.Add("      app ESTABLISHED -> " + ($remotes -join ', ') + " ; simulator-owned server ends: " + ($simOwned -join ', '))
            Check $case 'app connected to exactly 127.0.0.201..205:502' (($remotes -join ',') -eq ($simHosts -join ','))
            Check $case 'those five connections terminate in the simulator process' ((($simOwned) -join ',') -eq ($simHosts -join ','))
            $nonzero = @($t | Where-Object { $_ -match '^\[Modbus\]\[Read\] .* raw=([1-9]\d*) ' })
            $report.Add("      [Modbus][Read] lines=$(@($t | Where-Object { $_.Contains('[Modbus][Read]') }).Count) with raw!=0: $($nonzero.Count); e.g. " + ($nonzero | Select-Object -First 1))
            Check $case '[Modbus][Read] with non-zero raw value' ($nonzero.Count -gt 0)
        } else {
            Check $case 'no 127.0.0.20x in the log' (@($t | Where-Object { $_ -match '127\.0\.0\.20\d' }).Count -eq 0)
            Check $case 'no ESTABLISHED Modbus connection (192.168.1.x unreachable)' ($conns.Count -eq 0)
        }
    } finally {
        Remove-Item Env:\TAIDAFLOW_DEVICE_PROFILE -ErrorAction SilentlyContinue
        if ($sim -and -not $sim.HasExited) {
            $null = $sim.CloseMainWindow()
            if (-not $sim.WaitForExit(8000)) { Stop-Process -Id $sim.Id -Force }
        }
    }
    Start-Sleep -Seconds 1
    $left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 502, 8125 })
    Check $case 'no 502/8125 listener left after close' ($left.Count -eq 0)
}
$report | ForEach-Object { Write-Output $_ }
Write-Output ("verify_device_profile exit={0}" -f $(if ($fail) { 1 } else { 0 }))
if ($fail) { exit 1 }
exit 0

# w2-036 test helper (D1 read-failure case): freeze Adam60xxSimulator for a few seconds so
# that every Modbus request the desktop app sends times out while its TCP sessions stay
# connected to the simulator (closing the simulator instead would let the app reconnect
# to its own 0.0.0.0:502 aggregate server).  Only a process started by this test and named
# Adam60xxSimulator is accepted.  The action is recorded in ..\input-log.txt.
# Usage: powershell -ExecutionPolicy Bypass -File suspend_process.ps1 -ProcessId <pid> -Seconds 6
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [int]$Seconds = 6
)
$ErrorActionPreference = 'Stop'
$p = Get-Process -Id $ProcessId
if ($p.ProcessName -ne 'Adam60xxSimulator') { Write-Output "REFUSED: pid $ProcessId is $($p.ProcessName)"; exit 2 }
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W2036Nt {
    [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
    [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
}
"@
$log = Join-Path (Split-Path -Parent $PSScriptRoot) 'input-log.txt'
$t0 = (Get-Date).ToString('HH:mm:ss.fff')
$rc = [W2036Nt]::NtSuspendProcess($p.Handle)
Add-Content -Path $log -Value "[$t0] w2-036 suspend Adam60xxSimulator pid=$ProcessId (NtSuspendProcess rc=$rc) for ${Seconds}s : D1 read-failure case" -Encoding utf8
Start-Sleep -Seconds $Seconds
$rc2 = [W2036Nt]::NtResumeProcess($p.Handle)
$t1 = (Get-Date).ToString('HH:mm:ss.fff')
Add-Content -Path $log -Value "[$t1] w2-036 resume Adam60xxSimulator pid=$ProcessId (NtResumeProcess rc=$rc2)" -Encoding utf8
Write-Output "suspended $t0 .. resumed $t1 (rc=$rc/$rc2)"

# TaidaFlow - remove the Windows service TaidaFlowNginx registered by install-nginx-service.ps1.   (w2-076)
#
# FIELD machine, as ADMINISTRATOR:
#     powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\scripts\uninstall-nginx-service.ps1 -WhatIf
#     (the same without -WhatIf, in a cmd / PowerShell started with "Run as administrator")
# -WhatIf needs no administrator and changes nothing.
#
# What it does: stops the service (WinSW runs "nginx -p <nginx folder> -s quit"; waits up to -StopTimeoutSec),
# then removes it ("nginx\nginx-service.exe uninstall"; when nginx-service.xml is missing: "sc delete").
# NOT deleted: nginx (nginx\), conf\nginx.conf, nginx\nginx-service.xml, config.json, the log files.
# Afterwards nginx no longer starts with Windows; start-taidaflow.bat starts it again the old way
# ("cd nginx" + "start nginx") at the next start of TaidaFlow.
# A service TaidaFlowNginx registered from ANOTHER folder is not touched unless -Force is given.
#
# Usage: uninstall-nginx-service.ps1 [-WhatIf] [-StopTimeoutSec 60] [-Force]
# Exit codes: 0 removed, or not installed (nothing to do), or -WhatIf printed; 4 the service belongs to another
# folder (not removed; -Force removes it); 5 not an administrator (NOTHING changed); 6 removal failed;
# 7 the service did not stop within -StopTimeoutSec (open web pages keep nginx busy - stop TaidaFlow first).
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateRange(5, 3600)]
    [int]$StopTimeoutSec = 60,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { [Console]::Out.WriteLine($m) }
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
$whatIf = [bool]$WhatIfPreference
# -WhatIf is handled by this script itself ($whatIf); cmdlets below (module auto-load, Get-FileHash, ...) must not
# see it, or they would skip their own read-only work.
$WhatIfPreference = $false

if (-not $whatIf -and -not (Test-TaidaFlowIsAdministrator)) {
    Say "REFUSED: uninstall-nginx-service.ps1 removes the Windows service TaidaFlowNginx and needs an administrator."
    Say "  This PowerShell is not elevated - NOTHING was changed (the service, if any, was not stopped or removed)."
    Say "  Start cmd with right-click > 'Run as administrator' and run the same command again,"
    Say "  or add -WhatIf to only print what would be done (no administrator needed)."
    exit 5
}

$install = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$wrapper = Join-Path $install 'nginx\nginx-service.exe'
$xmlPath = Join-Path $install 'nginx\nginx-service.xml'
$name = $script:TaidaFlowNginxServiceName
$svc = Get-TaidaFlowNginxServiceInfo
if ($svc.Error) { Say "WARNING: $($svc.Error)" }
$mode = Get-TaidaFlowNginxServiceMode $svc $wrapper
Say "=== TaidaFlow nginx service - $(if ($whatIf) { '-WhatIf: nothing is changed' } else { 'uninstall' })"
Say "installation folder : $install"
if ($mode -eq 'none') { Say "service $name is not installed - nothing to do"; exit 0 }
Say "service $name       : $($svc.PathName) (state $($svc.State), start mode $($svc.StartMode))$(if ($mode -eq 'other') { ' - registered from ANOTHER folder' })"
$useWrapper = ($mode -eq 'ours') -and (Test-Path -LiteralPath $wrapper -PathType Leaf) -and (Test-Path -LiteralPath $xmlPath -PathType Leaf)
Say "steps$(if ($whatIf) { ' (NOT executed: -WhatIf)' }):"
$n = 1
if ($svc.State -ne 'Stopped') { Say "  $n. stop the service $name (= `"$wrapper`" stop: nginx -p <nginx folder> -s quit; wait up to $StopTimeoutSec s)"; $n++ }
if ($useWrapper) { Say "  $n. `"$wrapper`" uninstall" } else { Say "  $n. sc delete $name" }
Say "kept: nginx\ (nginx.exe, conf\nginx.conf, nginx-service.exe, nginx-service.xml), config.json, the log folder"
if ($mode -eq 'other' -and -not $Force) {
    Say "REFUSED: the service belongs to another folder ($($svc.ExePath)). Use the uninstall-nginx-service.ps1 of that folder,"
    Say "  or run this one again with -Force to remove it anyway. Nothing was changed."
    exit 4
}
if ($whatIf) { Say "-WhatIf: NOTHING changed (the service was not stopped or removed)."; exit 0 }

Add-Type -AssemblyName System.ServiceProcess
if ($svc.State -ne 'Stopped') {
    Say "--- stopping $name"
    $sc = New-Object System.ServiceProcess.ServiceController $name
    try { $sc.Stop() } catch { Say "  stop: $($_.Exception.Message)" }
    try { $sc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds($StopTimeoutSec)) }
    catch {
        Say "the service did not stop within $StopTimeoutSec s (nginx -s quit waits for open web pages / downloads) - NOT removed."
        Say "  Stop TaidaFlow first (stop-taidaflow.bat) and run this script again; if it still hangs: taskkill /F /IM nginx.exe (administrator)."
        exit 7
    }
    Say "  stopped"
}
Say "--- removing $name"
$ErrorActionPreference = 'Continue'
if ($useWrapper) { $o = @(& $wrapper uninstall 2>&1 | ForEach-Object { "$_" }) }
else { $o = @(& "$env:SystemRoot\System32\sc.exe" delete $name 2>&1 | ForEach-Object { "$_" }) }
$rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
foreach ($line in $o) { if ($line.Trim() -ne '') { Say "  | $line" } }
$after = Get-TaidaFlowNginxServiceInfo
if ($after.Exists) {
    Say "the service still exists after the removal (exit $rc; state $($after.State)) - it is deleted when the last program that has it open"
    Say "  (services.msc, Task Manager) is closed, or after a restart of Windows. Check with: sc query $name"
    exit 6
}
Say "removed: $name (exit $rc). nginx is no longer started by Windows; start-taidaflow.bat starts it with 'start nginx' again."
exit 0

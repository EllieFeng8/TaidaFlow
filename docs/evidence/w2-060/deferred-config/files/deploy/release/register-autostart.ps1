# TaidaFlow - register automatic start at user logon (Windows Task Scheduler).   (w2-057)
#
# FIELD machine only. Registers ONE scheduled task that runs start-taidaflow.ps1 (this folder) when
# the given user logs on. Run it once, as that user (or as an administrator for another user), on
# the plant PC - never on a development PC. Always try it first with -WhatIf: then nothing is
# registered and the task that WOULD be registered is printed.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File register-autostart.ps1 -WhatIf
#       [-DataDir <folder>] [-LogDir <folder>] [-UseNginx] [-Port 80] [-RestPort 18080] [-TaskName TaidaFlow]
#       [-User <DOMAIN\user>] [-DelaySec 30]
#   (the same without -WhatIf registers it; remove it again with unregister-autostart.ps1)
#
# Why a logon task and not a Windows service:
#   TaidaFlowApp.exe is the operator HMI - a Qt Quick window on the local screen. Windows services
#   run in session 0, which has no visible desktop: the window would never be seen, and Qt Quick
#   rendering there is not supported/tested. So the app must run in the operator's interactive
#   session, which "At log on" + "Run only when user is logged on" (LogonType Interactive) gives.
#   For an unattended restart after a power failure the PC must log that user on automatically
#   (Windows automatic sign-in) - a decision for the plant owner (see DEPLOY.md).
#   If a headless service is ever wanted (web page only, no local HMI): wrap the exe with a service
#   wrapper such as WinSW (XML: <executable>C:\TaidaFlow\...\TaidaFlowApp.exe</executable>,
#   <workingdirectory>C:\TaidaFlowData</workingdirectory> (the app also reads dataDir of config.json
#   itself), <env name="TAIDAFLOW_DOWNLOAD_PORT" value="80"/>, <env name="TAIDAFLOW_REST_PORT"
#   value="18080"/>, <stopparentprocessfirst>, logs) or NSSM ("nssm install TaidaFlow <exe>", then
#   AppDirectory, AppEnvironmentExtra, AppStdout/AppStderr, AppStopMethodWindow). nginx can be
#   wrapped the same way (arguments -p <runtime>/ -c conf/taidaflow.conf, stop: -s quit). This is
#   NOT done or tested in this project; a service in session 0 has no local window at all.
#
# The task: action = powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden
#   -File "<this folder>\start-taidaflow.ps1" [-DataDir "<DataDir>"] [-LogDir "<LogDir>"] [-UseNginx[:$false]]
#   [-Port <n>] [-RestPort <n>] - only the parameters given HERE are written into the task (w2-060):
#   everything else comes from config.json next to start-taidaflow.ps1 at every start, so a later
#   change of config.json needs no new registration. The settings the task would use now are printed,
#   working directory = this folder; trigger = at logon of -User, delayed by -DelaySec (network,
#   drivers); principal = that user, Interactive, RunLevel Limited (no administrator rights needed:
#   ports 80/502/8124/8125 can be opened without them); settings = start also on battery, ignore
#   a second start while running, stop the start script after 10 minutes (the app itself is not
#   limited: start-taidaflow.ps1 ends as soon as the app runs).
#
# Exit codes: 0 registered (or -WhatIf printed); 2 wrong parameter / start script missing;
# 3 registration failed; 4 a task with this name already exists (use -Replace to overwrite it).
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$DataDir = "",
    [string]$LogDir = "",
    [switch]$UseNginx,
    [ValidateRange(1, 65535)]
    [int]$Port = 80,
    [ValidateRange(0, 65535)]
    [int]$RestPort = 0,
    [string]$TaskName = "TaidaFlow",
    [string]$User = "",
    [ValidateRange(0, 3600)]
    [int]$DelaySec = 30,
    [switch]$Replace
)
$ErrorActionPreference = 'Stop'
$install = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $install $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
function Say([string]$m) { [Console]::Out.WriteLine($m) }

$start = Join-Path $install 'start-taidaflow.ps1'
if (-not (Test-Path $start -PathType Leaf)) { Say "start script not found: $start"; exit 2 }
# w2-060: config.json (next to start-taidaflow.ps1) is the site configuration; only parameters given
# here override it (and are written into the task).
$cfgReader = Join-Path $install 'scripts\taidaflow-config.ps1'
if (-not (Test-Path $cfgReader -PathType Leaf)) { Say "missing: $cfgReader (incomplete package)"; exit 2 }
. $cfgReader
$cfg = Read-TaidaFlowConfig $install
if ($cfg.Error) { Say "config.json error: $($cfg.Error)"; exit 2 }
$dataGiven = $PSBoundParameters.ContainsKey('DataDir') -and $DataDir -ne ''
if ($dataGiven) { $DataDir = Full $DataDir }
$effData = if ($dataGiven) { "$DataDir (-DataDir)" } elseif ($cfg.DataDir) { "$($cfg.DataDir) (config.json)" } else { "$(Join-Path $install 'runtime') (default, no config.json dataDir)" }
$effNginx = if ($PSBoundParameters.ContainsKey('UseNginx')) { "$([bool]$UseNginx) (-UseNginx)" } elseif ($null -ne $cfg.UseNginx) { "$($cfg.UseNginx) (config.json)" } else { 'False (default)' }
$effPort = if ($PSBoundParameters.ContainsKey('Port')) { "$Port (-Port)" } elseif ($cfg.NginxPort) { "$($cfg.NginxPort) (config.json)" } else { '80 (default)' }
$effRest = if ($RestPort -gt 0) { "$RestPort (-RestPort)" } elseif ($cfg.RestPort) { "$($cfg.RestPort) (config.json)" } else { '18080 (default)' }
if ($LogDir -ne "") { $LogDir = Full $LogDir }
if ($User -eq "") { $User = "$env:USERDOMAIN\$env:USERNAME" }
foreach ($v in $start, $DataDir, $LogDir) {
    if ($v -match '"') { Say "path must not contain a double quote: $v"; exit 2 }
}

# Every path is quoted, so folders with spaces stay one argument each.
$arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $start + '"'
if ($dataGiven) { $arguments += ' -DataDir "' + $DataDir + '"' }
if ($LogDir -ne "") { $arguments += ' -LogDir "' + $LogDir + '"' }
if ($PSBoundParameters.ContainsKey('UseNginx')) { $arguments += $(if ($UseNginx) { ' -UseNginx' } else { ' -UseNginx:$false' }) }
if ($PSBoundParameters.ContainsKey('Port')) { $arguments += " -Port $Port" }
if ($RestPort -gt 0) { $arguments += " -RestPort $RestPort" }
$execute = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

# The objects below are built in memory only (New-ScheduledTask* do not register anything).
$action = New-ScheduledTaskAction -Execute $execute -Argument $arguments -WorkingDirectory $install
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $User
if ($DelaySec -gt 0) { $trigger.Delay = 'PT' + $DelaySec + 'S' }
$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
                -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
$description = "TaidaFlow field start at logon of $User (start-taidaflow.ps1, settings from config.json$(if ($dataGiven) { ", data folder $DataDir" })). " +
               "Registered by register-autostart.ps1; remove with unregister-autostart.ps1."
$task = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description $description

$existing = $null
try { $existing = Get-ScheduledTask -TaskName $TaskName -TaskPath '\' -ErrorAction Stop } catch { $existing = $null }

Say "scheduled task definition (registered only without -WhatIf):"
Say "  name              : \$TaskName"
Say "  description       : $description"
Say "  trigger           : at log on of $User, delay $(if ($DelaySec -gt 0) { $trigger.Delay } else { 'none' })"
Say "  run as            : $($principal.UserId), logon type $($principal.LogonType) (only when the user is logged on - the HMI window needs the desktop), run level $($principal.RunLevel)"
Say "  action execute    : $($action.Execute)"
Say "  action arguments  : $($action.Arguments)"
Say "  config.json       : $(if ($cfg.Found) { $cfg.Path } else { "$($cfg.Path) not found (built-in defaults)" })"
foreach ($n in $cfg.Notes) { Say "  config.json note  : $n" }
Say "  settings now      : dataDir $effData; useNginx $effNginx; nginxPort $effPort; restPort $effRest"
Say "  working directory : $($action.WorkingDirectory)"
Say "  settings          : MultipleInstances=$($settings.MultipleInstances) ExecutionTimeLimit=$($settings.ExecutionTimeLimit) StartWhenAvailable=$($settings.StartWhenAvailable) DisallowStartIfOnBatteries=$($settings.DisallowStartIfOnBatteries) StopIfGoingOnBatteries=$($settings.StopIfGoingOnBatteries)"
Say "  existing task     : $(if ($existing) { "YES (state $($existing.State)) - $(if ($Replace) { 'would be replaced (-Replace)' } else { 'refused without -Replace' })" } else { 'none' })"

if (-not $PSCmdlet.ShouldProcess("\$TaskName (Task Scheduler, user $User)", 'Register-ScheduledTask')) {
    Say "-WhatIf: NOTHING registered."
    exit 0
}
if ($existing -and -not $Replace) { Say "a task named \$TaskName already exists - not changed (use -Replace, or unregister-autostart.ps1 first)"; exit 4 }
try {
    if ($existing) { Register-ScheduledTask -TaskName $TaskName -TaskPath '\' -InputObject $task -Force | Out-Null }
    else { Register-ScheduledTask -TaskName $TaskName -TaskPath '\' -InputObject $task | Out-Null }
} catch {
    Say "registration FAILED: $($_.Exception.Message)"
    Say "(registering a task for another user needs an administrator)"
    exit 3
}
Say "registered: \$TaskName - TaidaFlow starts at the next log on of $User (test now: Start-ScheduledTask -TaskName $TaskName)"
exit 0

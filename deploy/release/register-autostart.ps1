# TaidaFlow - register automatic start at user logon (Windows Task Scheduler).   (w2-057, w2-062, w2-065)
#
# FIELD machine only. Registers ONE scheduled task that runs start-taidaflow.ps1 (this folder) when
# the given user logs on. Run it once, as that user (or as an administrator for another user), on
# the plant PC - never on a development PC. Always try it first with -WhatIf: then nothing is
# registered and the task that WOULD be registered is printed.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File register-autostart.ps1 -WhatIf
#       [-Config <config.json>] [-LogDir <folder>] [-TaskName TaidaFlow]
#       [-User <DOMAIN\user>] [-DelaySec 30]
#   (the same without -WhatIf registers it; remove it again with unregister-autostart.ps1)
#
# Why a logon task and not a Windows service:
#   TaidaFlowApp.exe is the operator HMI - a Qt Quick window on the local screen. Windows services
#   run in session 0, which has no visible desktop: the window would never be seen, and Qt Quick
#   rendering there is not supported/tested. So the app must run in the operator's interactive
#   session, which "At log on" + "Run only when user is logged on" (LogonType Interactive) gives.
#   For an unattended restart after a power failure the PC must log that user on automatically
#   (Windows automatic sign-in) - set up on site by the plant (decided, see DEPLOY.md section 7);
#   this script does not touch it.
#   If a headless service is ever wanted (web page only, no local HMI): wrap the exe with a service
#   wrapper such as WinSW (XML: <executable>C:\TaidaFlow\...\TaidaFlowApp.exe</executable>, the
#   settings in config.json next to it, <stopparentprocessfirst>, logs) or NSSM ("nssm install TaidaFlow <exe>", then
#   AppDirectory, AppEnvironmentExtra, AppStdout/AppStderr, AppStopMethodWindow). nginx can be
#   wrapped the same way (arguments -p <runtime>/ -c conf/taidaflow.conf, stop: -s quit). This is
#   NOT done or tested in this project; a service in session 0 has no local window at all.
#
# The task: action = powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden
#   -File "<this folder>\start-taidaflow.ps1" [-Config "<config.json>"] [-LogDir "<LogDir>"],
#   working directory = this folder; trigger = at logon of -User, delayed by -DelaySec (network,
#   drivers); principal = that user, Interactive, RunLevel Limited (no administrator rights needed:
#   the ports of config.json above 1024 and 80/502 can be opened without them); settings = start
#   also on battery, ignore a second start while running, stop the start script after 10 minutes
#   (the app itself is not limited: start-taidaflow.ps1 ends as soon as the app runs).
# w2-062: the data folder, nginx and every port come from config.json at EACH start (edit config.json,
#   the task stays as it is). The task passes no setting of its own; -Config only when the file is not
#   <this folder>\config.json. A missing config.json is created now with the defaults (not with -WhatIf).
# w2-065: logs. The app writes its own log files into config.json log.dir (default C:\TaidaFlowData\logs),
#   start-taidaflow.ps1 its launcher-YYYY-MM-DD.log into the same folder. -LogDir is passed to
#   start-taidaflow.ps1 as its one-time override of log.dir at EVERY start (app, nginx and launcher logs
#   then all go there); normally leave it out and set log.dir in config.json instead.
#
# Exit codes: 0 registered (or -WhatIf printed); 2 wrong parameter / start script missing /
# config.json unusable; 3 registration failed; 4 a task with this name already exists (use -Replace).
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$Config = "",
    [string]$LogDir = "",
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
. (Join-Path $install 'scripts\taidaflow-config.ps1')
$configPath = if ($Config -ne "") { Full $Config } else { Join-Path $install 'config.json' }
# Only without -WhatIf a missing config.json is written (TaidaFlowApp.exe --write-default-config).
$create = -not $WhatIfPreference
$cfg = Get-TaidaFlowConfig -Path $configPath -Exe (Join-Path $install 'TaidaFlowApp.exe') -Create:$create
if ($cfg.Error) { Say "config.json unusable: $($cfg.Error)"; exit 2 }
$DataDir = $cfg.DataDir
if ($LogDir -ne "") { $LogDir = Full $LogDir }
if ($User -eq "") { $User = "$env:USERDOMAIN\$env:USERNAME" }
foreach ($v in $start, $configPath, $LogDir) {
    if ($v -match '"') { Say "path must not contain a double quote: $v"; exit 2 }
}

# Every path is quoted, so folders with spaces stay one argument each.
$arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $start + '"'
if ($Config -ne "") { $arguments += ' -Config "' + $configPath + '"' }
if ($LogDir -ne "") { $arguments += ' -LogDir "' + $LogDir + '"' }
$execute = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

# The objects below are built in memory only (New-ScheduledTask* do not register anything).
$action = New-ScheduledTaskAction -Execute $execute -Argument $arguments -WorkingDirectory $install
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $User
if ($DelaySec -gt 0) { $trigger.Delay = 'PT' + $DelaySec + 'S' }
$principal = New-ScheduledTaskPrincipal -UserId $User -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
                -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
$description = "TaidaFlow field start at logon of $User (start-taidaflow.ps1, data folder $DataDir). " +
               "Registered by register-autostart.ps1; remove with unregister-autostart.ps1."
$task = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description $description

$existing = $null
try { $existing = Get-ScheduledTask -TaskName $TaskName -TaskPath '\' -ErrorAction Stop } catch { $existing = $null }

Say "config.json       : $configPath$(if ($cfg.Created) { ' (CREATED now with the default values)' } elseif (-not $cfg.Exists) { ' (missing - start-taidaflow.ps1 creates it with the defaults)' })"
foreach ($n in $cfg.Notes) { Say "  config: $n" }
Say ("  settings used at each start: data folder {0}, log folder {5}, nginx {1} (port {2}, {3}), devices {4}" -f $DataDir,
     $(if ($cfg.Values['nginx.enabled']) { 'on' } else { 'off' }), $cfg.Values['nginx.port'], $cfg.Values['nginx.exe'],
     ((@('adam6256', 'adam6217a', 'adam6217b', 'adam6224', 'adam6022') | ForEach-Object { $cfg.Values["devices.$_.host"] }) -join ', '),
     $(if ($LogDir -ne "") { "$LogDir (-LogDir, overrides log.dir)" } else { "$(Resolve-TaidaFlowLogDir $cfg) (config.json log.dir = $($cfg.Values['log.dir']))" }))
Say "scheduled task definition (registered only without -WhatIf):"
Say "  name              : \$TaskName"
Say "  description       : $description"
Say "  trigger           : at log on of $User, delay $(if ($DelaySec -gt 0) { $trigger.Delay } else { 'none' })"
Say "  run as            : $($principal.UserId), logon type $($principal.LogonType) (only when the user is logged on - the HMI window needs the desktop), run level $($principal.RunLevel)"
Say "  action execute    : $($action.Execute)"
Say "  action arguments  : $($action.Arguments)"
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

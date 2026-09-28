# TaidaFlow - remove the automatic start registered by register-autostart.ps1.   (w2-057)
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File unregister-autostart.ps1 -WhatIf [-TaskName TaidaFlow]
#        (the same without -WhatIf removes the task)
# Only a task whose action runs start-taidaflow.ps1 is removed (a different task that happens to
# have the same name is left alone, exit 4). The running app is NOT stopped - use
# stop-taidaflow.ps1 for that.
# Exit codes: 0 removed (or -WhatIf printed); 1 no such task; 3 removal failed;
# 4 the task does not run start-taidaflow.ps1 (not removed).
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$TaskName = "TaidaFlow"
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { [Console]::Out.WriteLine($m) }

$task = $null
try { $task = Get-ScheduledTask -TaskName $TaskName -TaskPath '\' -ErrorAction Stop } catch { $task = $null }
if (-not $task) { Say "no scheduled task \$TaskName - nothing to remove"; exit 1 }
$actions = @($task.Actions | ForEach-Object { "$($_.Execute) $($_.Arguments)" })
Say "scheduled task \$TaskName (state $($task.State)):"
$actions | ForEach-Object { Say "  action: $_" }
if (-not (@($actions | Where-Object { $_ -match 'start-taidaflow\.ps1' }).Count)) {
    Say "this task does not run start-taidaflow.ps1 - not removed"
    exit 4
}
if (-not $PSCmdlet.ShouldProcess("\$TaskName", 'Unregister-ScheduledTask')) {
    Say "-WhatIf: NOTHING removed."
    exit 0
}
try { Unregister-ScheduledTask -TaskName $TaskName -TaskPath '\' -Confirm:$false }
catch { Say "removal FAILED: $($_.Exception.Message)"; exit 3 }
Say "removed: \$TaskName (TaidaFlow no longer starts at log on; the running app was not touched)"
exit 0

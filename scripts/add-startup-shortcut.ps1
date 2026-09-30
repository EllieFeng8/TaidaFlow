# TaidaFlow - start TaidaFlow at log on through the Startup folder (shortcut "TaidaFlow").        (w2-076)
#
# FIELD machine (Mango 2026-09-29: "register the program in the Startup folder"). Creates the shortcut
#     <Startup folder>\TaidaFlow.lnk
#       target      : C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe
#       arguments   : -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "<installation folder>\start-taidaflow.ps1"
#                     [-Config "<config.json>"]
#       start in    : <installation folder>
#       run         : minimized (no window stays open; start-taidaflow.ps1 keeps its single-instance and port
#                     checks and writes launcher-YYYY-MM-DD.log in the log folder)
# Startup folder: the current user's (shell:startup, no administrator needed); -AllUsers: every user's
# (shell:common startup, administrator). -ShortcutDir <folder>: another folder (tests only).
# Use ONE automatic start: this shortcut OR the Task Scheduler task of register-autostart.ps1. When the other
# one is found it is reported (never removed automatically - unregister-autostart.ps1 removes the task).
# nginx is not started by the shortcut itself: start-taidaflow.ps1 uses the nginx service TaidaFlowNginx when
# it is installed (scripts\install-nginx-service.ps1), else starts nginx with "start nginx" as before.
#
# Usage: add-startup-shortcut.ps1 [-WhatIf] [-AllUsers | -ShortcutDir <folder>] [-Config <config.json>]
# Exit codes: 0 created / updated / already up to date (or -WhatIf printed); 2 wrong parameter, start script
# or folder missing, path with a double quote; 3 creating the shortcut failed; 5 -AllUsers without administrator.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$AllUsers,
    [string]$ShortcutDir = "",
    [string]$Config = ""
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { [Console]::Out.WriteLine($m) }
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
$whatIf = [bool]$WhatIfPreference
$install = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path (Get-Location).Path $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
if ($AllUsers -and $ShortcutDir -ne "") { Say "-AllUsers and -ShortcutDir together - use one"; exit 2 }
if ($AllUsers -and -not $whatIf -and -not (Test-TaidaFlowIsAdministrator)) {
    Say "REFUSED: -AllUsers writes into the Startup folder of all users and needs an administrator - nothing changed."
    Say "  Without -AllUsers the shortcut goes into YOUR Startup folder (no administrator needed)."
    exit 5
}
$start = Join-Path $install 'start-taidaflow.ps1'
if (-not (Test-Path -LiteralPath $start -PathType Leaf)) { Say "start script not found: $start"; exit 2 }
$dir = if ($ShortcutDir -ne "") { Full $ShortcutDir } elseif ($AllUsers) { [Environment]::GetFolderPath('CommonStartup') } else { [Environment]::GetFolderPath('Startup') }
$scope = if ($ShortcutDir -ne "") { '-ShortcutDir (test folder)' } elseif ($AllUsers) { 'all users (shell:common startup)' } else { "user $env:USERDOMAIN\$env:USERNAME (shell:startup)" }
if (-not $dir -or -not (Test-Path -LiteralPath $dir -PathType Container)) { Say "Startup folder not found: '$dir'"; exit 2 }
$lnkPath = Join-Path $dir 'TaidaFlow.lnk'
$configPath = if ($Config -ne "") { Full $Config } else { '' }
foreach ($x in $start, $configPath, $install) { if ($x -match '"') { Say "path must not contain a double quote: $x"; exit 2 } }
$target = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $start + '"'
if ($configPath -ne '') { $arguments += ' -Config "' + $configPath + '"' }
$icon = (Join-Path $install 'TaidaFlowApp.exe') + ',0'
$description = "TaidaFlow field start at log on (start-taidaflow.ps1 of $install). Created by scripts\add-startup-shortcut.ps1; remove with scripts\remove-startup-shortcut.ps1."

$shell = New-Object -ComObject WScript.Shell
function Read-Lnk([string]$p) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $null }
    $s = $shell.CreateShortcut($p)
    return [pscustomobject]@{ TargetPath = $s.TargetPath; Arguments = $s.Arguments; WorkingDirectory = $s.WorkingDirectory; WindowStyle = $s.WindowStyle; Description = $s.Description; IconLocation = $s.IconLocation }
}
$existing = Read-Lnk $lnkPath
$same = $existing -and [string]::Equals($existing.TargetPath, $target, [System.StringComparison]::OrdinalIgnoreCase) -and $existing.Arguments -ceq $arguments -and
        [string]::Equals($existing.WorkingDirectory, $install, [System.StringComparison]::OrdinalIgnoreCase) -and $existing.WindowStyle -eq 7

Say "=== TaidaFlow Startup shortcut - $(if ($whatIf) { '-WhatIf: nothing is changed' } else { 'add' })"
Say "shortcut    : $lnkPath ($scope)"
Say "target      : $target"
Say "arguments   : $arguments"
Say "start in    : $install"
Say "run         : minimized (WindowStyle 7); icon $icon"
if ($existing) {
    Say "existing    : $(if ($same) { 'yes, identical - nothing to do' } else { 'yes, DIFFERENT - will be replaced' })"
    if (-not $same) { Say "  old: target $($existing.TargetPath) | arguments $($existing.Arguments) | start in $($existing.WorkingDirectory) | window $($existing.WindowStyle)" }
} else { Say "existing    : none" }

# The other automatic start (report only, never removed here).
$others = New-Object System.Collections.Generic.List[string]
try {
    foreach ($t in @(Get-ScheduledTask -ErrorAction Stop)) {
        $acts = @($t.Actions | ForEach-Object { "$($_.Execute) $($_.Arguments)" })
        if ($t.TaskName -eq 'TaidaFlow' -or @($acts | Where-Object { $_ -match 'start-taidaflow\.ps1' }).Count) {
            $others.Add("Task Scheduler task $($t.TaskPath)$($t.TaskName) (state $($t.State)): $($acts -join ' ; ')")
        }
    }
} catch { Say "note: the Task Scheduler could not be read ($($_.Exception.Message)) - check it by hand (taskschd.msc)" }
foreach ($other in @([Environment]::GetFolderPath('Startup'), [Environment]::GetFolderPath('CommonStartup'))) {
    if (-not $other) { continue }
    $o = Join-Path $other 'TaidaFlow.lnk'
    if ([string]::Equals($o, $lnkPath, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
    if (Test-Path -LiteralPath $o -PathType Leaf) { $others.Add("another Startup shortcut: $o") }
}
if ($others.Count) {
    Say "NOTE: another automatic start of TaidaFlow is set up - use only ONE (both would run start-taidaflow at log on;"
    Say "      the second start is refused with exit 4 but it is not intended). It is NOT removed automatically:"
    foreach ($x in $others) { Say "        $x" }
    Say "      remove the task with unregister-autostart.ps1, a shortcut with scripts\remove-startup-shortcut.ps1 [-AllUsers]."
} else { Say "other automatic start: none found (Task Scheduler, the other Startup folder)" }

if ($same) { Say "the shortcut is already up to date - nothing changed"; exit 0 }
if (-not $PSCmdlet.ShouldProcess($lnkPath, 'create the Startup shortcut')) { Say "-WhatIf: NOTHING changed (no shortcut written)."; exit 0 }
try {
    $s = $shell.CreateShortcut($lnkPath)
    $s.TargetPath = $target
    $s.Arguments = $arguments
    $s.WorkingDirectory = $install
    $s.WindowStyle = 7
    $s.IconLocation = $icon
    $s.Description = $description
    $s.Save()
} catch { Say "creating the shortcut FAILED: $($_.Exception.Message)"; exit 3 }
$back = Read-Lnk $lnkPath
if (-not $back -or $back.Arguments -cne $arguments -or -not [string]::Equals($back.TargetPath, $target, [System.StringComparison]::OrdinalIgnoreCase)) { Say "the shortcut was written but reads back differently: $($back | Out-String)"; exit 3 }
Say "written: $lnkPath"
Say "  read back: target $($back.TargetPath) | arguments $($back.Arguments) | start in $($back.WorkingDirectory) | window $($back.WindowStyle)"
Say "TaidaFlow starts at the next log on$(if ($ShortcutDir -eq '') { " of $(if ($AllUsers) { 'every user' } else { $env:USERNAME })" }) (test now: double-click the shortcut). Remove: scripts\remove-startup-shortcut.ps1$(if ($AllUsers) { ' -AllUsers' })"
exit 0

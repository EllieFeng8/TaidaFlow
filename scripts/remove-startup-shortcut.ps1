# TaidaFlow - remove the Startup folder shortcut "TaidaFlow" of add-startup-shortcut.ps1.           (w2-076)
#
# Usage: remove-startup-shortcut.ps1 [-WhatIf] [-AllUsers | -ShortcutDir <folder>] [-Force]
#   default    : the current user's Startup folder (shell:startup)
#   -AllUsers  : the Startup folder of all users (shell:common startup, administrator)
#   -ShortcutDir: another folder (tests only)
# Only a shortcut that runs start-taidaflow.ps1 is removed (another shortcut named TaidaFlow.lnk is left alone,
# exit 4; -Force removes it anyway). The running app is NOT stopped (stop-taidaflow.bat), the Task Scheduler
# task of register-autostart.ps1 is not touched (unregister-autostart.ps1).
# Exit codes: 0 removed (or -WhatIf printed); 1 no shortcut; 2 wrong parameter / folder missing; 3 removal failed;
# 4 the shortcut does not run start-taidaflow.ps1 (not removed); 5 -AllUsers without administrator.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$AllUsers,
    [string]$ShortcutDir = "",
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { [Console]::Out.WriteLine($m) }
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
$whatIf = [bool]$WhatIfPreference
if ($AllUsers -and $ShortcutDir -ne "") { Say "-AllUsers and -ShortcutDir together - use one"; exit 2 }
if ($AllUsers -and -not $whatIf -and -not (Test-TaidaFlowIsAdministrator)) {
    Say "REFUSED: -AllUsers changes the Startup folder of all users and needs an administrator - nothing changed."
    exit 5
}
$dir = if ($ShortcutDir -ne "") {
    $p = $ShortcutDir; if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path (Get-Location).Path $p }
    [System.IO.Path]::GetFullPath($p).TrimEnd('\')
} elseif ($AllUsers) { [Environment]::GetFolderPath('CommonStartup') } else { [Environment]::GetFolderPath('Startup') }
if (-not $dir -or -not (Test-Path -LiteralPath $dir -PathType Container)) { Say "Startup folder not found: '$dir'"; exit 2 }
$lnkPath = Join-Path $dir 'TaidaFlow.lnk'
if (-not (Test-Path -LiteralPath $lnkPath -PathType Leaf)) { Say "no shortcut $lnkPath - nothing to remove"; exit 1 }
$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnkPath)
Say "shortcut  : $lnkPath"
Say "target    : $($s.TargetPath)"
Say "arguments : $($s.Arguments)"
Say "start in  : $($s.WorkingDirectory)"
$ours = "$($s.TargetPath) $($s.Arguments)" -match 'start-taidaflow\.ps1'
if (-not $ours -and -not $Force) { Say "this shortcut does not run start-taidaflow.ps1 - not removed (-Force removes it anyway)"; exit 4 }
if (-not $PSCmdlet.ShouldProcess($lnkPath, 'remove the Startup shortcut')) { Say "-WhatIf: NOTHING removed."; exit 0 }
try { Remove-Item -LiteralPath $lnkPath -Force } catch { Say "removal FAILED: $($_.Exception.Message)"; exit 3 }
if (Test-Path -LiteralPath $lnkPath) { Say "removal FAILED: $lnkPath still exists"; exit 3 }
Say "removed: $lnkPath (TaidaFlow no longer starts from this Startup folder; the running app was not touched)"
exit 0

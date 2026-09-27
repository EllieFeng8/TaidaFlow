# w2-053 test helper: set one ADAM-6224 DI checkbox of the running Adam60xxSimulator through
# Windows UI Automation (TogglePattern) - no mouse move, no key press, no window activation.
# Only the simulator process is touched (found by process name, exactly one instance required);
# the TaidaFlow window is never addressed.  Every action is appended with a millisecond
# timestamp to docs\evidence\w2-053\input-log.txt.
#
#   powershell -ExecutionPolicy Bypass -File sim_di.ps1 -Di DI1 -Value 1 -Note "scenario A raise"
#   powershell -ExecutionPolicy Bypass -File sim_di.ps1 -Di DI1 -Query      (read only)
# Exit 0 = checkbox is at the requested value afterwards (or -Query printed it);
#      2 = simulator/window/checkbox not found; 3 = value did not change.
param(
    [ValidateSet('DI0', 'DI1', 'DI2')][string]$Di = 'DI1',
    [ValidateSet('0', '1')][string]$Value = '1',
    [string]$Note = "",
    [switch]$Query
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$evidence = Split-Path -Parent $PSScriptRoot
$logFile = Join-Path $evidence 'input-log.txt'
function Log([string]$text) {
    $line = "[" + (Get-Date).ToString('HH:mm:ss.fff') + "] w2-053 sim " + $text
    Add-Content -Path $logFile -Value $line -Encoding utf8
    Write-Output $line
}

$procs = @(Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($procs.Count -ne 1) { Log "$Di : expected exactly 1 Adam60xxSimulator process, found $($procs.Count)"; exit 2 }
$A = [System.Windows.Automation.AutomationElement]
$root = $A::RootElement
$pidCond = New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procs[0].Id)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $pidCond)
if (-not $win) { Log "$Di : simulator window (pid $($procs[0].Id)) not found"; exit 2 }
$cbCond = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::CheckBox)
$boxes = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cbCond)
$box = $null
foreach ($b in $boxes) { if ($b.Current.Name.StartsWith("$Di ")) { $box = $b; break } }
if (-not $box) { Log "$Di : checkbox '$Di ...' not found in simulator pid $($procs[0].Id)"; exit 2 }
$toggle = $box.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
$before = if ($toggle.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On) { '1' } else { '0' }
if ($Query) { Log "$Di query: value=$before : $Note"; exit 0 }
if ($before -ne $Value) { $toggle.Toggle() }
Start-Sleep -Milliseconds 200
$after = if ($toggle.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On) { '1' } else { '0' }
$code = if ($after -eq $Value) { 0 } else { 3 }
Log "$Di $before -> $after (requested $Value, UIA TogglePattern, pid $($procs[0].Id)) : $Note (exit $code)"
exit $code

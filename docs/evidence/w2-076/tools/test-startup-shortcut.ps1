# w2-076 test of <package>\scripts\add-startup-shortcut.ps1 / remove-startup-shortcut.ps1 with -ShortcutDir =
# a TEST folder (build\w2-076-startup-test\Startup). The real Startup folders (shell:startup, shell:common startup)
# are only listed before and after (must be unchanged) - never written. No administrator.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-076\tools\test-startup-shortcut.ps1 -Package dist\TaidaFlow-<...>
# Exit 0 = all checks passed.
param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
if (-not [System.IO.Path]::IsPathRooted($Package)) { $Package = Join-Path $root $Package }
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
$add = Join-Path $Package 'scripts\add-startup-shortcut.ps1'
$remove = Join-Path $Package 'scripts\remove-startup-shortcut.ps1'
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$testRoot = Join-Path $root 'build\w2-076-startup-test'
$dir = Join-Path $testRoot 'Startup'
$lnk = Join-Path $dir 'TaidaFlow.lnk'
$script:fail = 0; $script:n = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    $script:n++; if (-not $ok) { $script:fail++ }
    [Console]::Out.WriteLine(("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' })))
}
function Run([string]$script, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $out = @(& $ps -NoProfile -ExecutionPolicy Bypass -File $script @arguments 2>&1 | ForEach-Object { "$_" })
    $rc = $LASTEXITCODE
    [Console]::Out.WriteLine("  > $(Split-Path -Leaf $script) $($arguments -join ' ')   (exit $rc)")
    foreach ($l in $out) { [Console]::Out.WriteLine("  | $l") }
    return [pscustomobject]@{ rc = $rc; out = ($out -join "`n") }
}
function Listing([string]$d) { if ($d -and (Test-Path -LiteralPath $d)) { return ((@(Get-ChildItem -LiteralPath $d -Force | ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) | Sort-Object) -join ';') } return '<none>' }
$realUser = [Environment]::GetFolderPath('Startup'); $realAll = [Environment]::GetFolderPath('CommonStartup')
$before = @{ user = (Listing $realUser); all = (Listing $realAll) }
[Console]::Out.WriteLine("=== w2-076 Startup shortcut test $((Get-Date).ToString('o')); package $Package")
[Console]::Out.WriteLine("real Startup folders (only listed): user $realUser = [$($before.user)]; all users $realAll = [$($before.all)]")
if (Test-Path $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
New-Item -ItemType Directory -Force $dir | Out-Null
$shell = New-Object -ComObject WScript.Shell
$expTarget = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$expArgs = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $Package 'start-taidaflow.ps1') + '"'

$r = Run $add @('-WhatIf', '-ShortcutDir', $dir)
Check '1 add -WhatIf: exit 0, prints target / arguments, NO shortcut written' ($r.rc -eq 0 -and $r.out -match '-WhatIf: NOTHING changed' -and -not (Test-Path $lnk)) ''
$r = Run $add @('-ShortcutDir', $dir)
Check '2 add: exit 0, TaidaFlow.lnk written in the test folder' ($r.rc -eq 0 -and (Test-Path $lnk)) ''
if (Test-Path $lnk) {
    $s = $shell.CreateShortcut($lnk)
    [Console]::Out.WriteLine("  read back: TargetPath=$($s.TargetPath) | Arguments=$($s.Arguments) | WorkingDirectory=$($s.WorkingDirectory) | WindowStyle=$($s.WindowStyle) | IconLocation=$($s.IconLocation)")
    Check '3 read back: target = powershell.exe (System32, full path)' ([string]::Equals($s.TargetPath, $expTarget, [System.StringComparison]::OrdinalIgnoreCase)) $s.TargetPath
    Check '4 read back: arguments = -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "<package>\start-taidaflow.ps1"' ($s.Arguments -ceq $expArgs) $s.Arguments
    Check '5 read back: start in = the installation (package) folder' ([string]::Equals($s.WorkingDirectory, $Package, [System.StringComparison]::OrdinalIgnoreCase)) $s.WorkingDirectory
    Check '6 read back: run minimized (WindowStyle 7)' ($s.WindowStyle -eq 7) "$($s.WindowStyle)"
}
$r = Run $add @('-ShortcutDir', $dir)
Check '7 add again (idempotent): exit 0, "already up to date"' ($r.rc -eq 0 -and $r.out -match 'already up to date') ''
Check '8 add reports the other automatic start (Task Scheduler / other Startup folder) - none on this PC' ($r.out -match 'other automatic start: none found|NOTE: another automatic start') ''
$r = Run $add @('-AllUsers')
Check '9 add -AllUsers without administrator: exit 5, all-users Startup folder unchanged' ($r.rc -eq 5 -and (Listing $realAll) -eq $before.all) ''
$r = Run $remove @('-WhatIf', '-ShortcutDir', $dir)
Check '10 remove -WhatIf: exit 0, shortcut still there' ($r.rc -eq 0 -and (Test-Path $lnk)) ''
$r = Run $remove @('-ShortcutDir', $dir)
Check '11 remove: exit 0, shortcut gone' ($r.rc -eq 0 -and -not (Test-Path $lnk)) ''
$r = Run $remove @('-ShortcutDir', $dir)
Check '12 remove again: exit 1 (no shortcut)' ($r.rc -eq 1) ''
$f = $shell.CreateShortcut($lnk); $f.TargetPath = Join-Path $env:SystemRoot 'System32\notepad.exe'; $f.Save()
$r = Run $remove @('-ShortcutDir', $dir)
Check '13 a TaidaFlow.lnk that does not run start-taidaflow.ps1: exit 4, not removed' ($r.rc -eq 4 -and (Test-Path $lnk)) ''
Remove-Item -LiteralPath $testRoot -Recurse -Force
$after = @{ user = (Listing $realUser); all = (Listing $realAll) }
Check '14 real Startup folders unchanged (user and all users)' ($after.user -eq $before.user -and $after.all -eq $before.all) "user [$($after.user)] all [$($after.all)]"
[Console]::Out.WriteLine("=== $($script:n) check(s), $($script:fail) failed")
if ($script:fail) { exit 1 }
exit 0

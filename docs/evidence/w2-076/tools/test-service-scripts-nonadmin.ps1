# w2-076 test of <package>\scripts\install-nginx-service.ps1 / uninstall-nginx-service.ps1 WITHOUT administrator
# (this PC, never elevated; no service is installed, nothing outside build\ is written):
#   1-2  install / uninstall without -WhatIf -> exit 5 "REFUSED ... needs an administrator", and NOTHING changed:
#        the package folder (every file, size, time) and C:\TaidaFlowData are the same before and after, no
#        nginx-service.xml, no config.json, no service TaidaFlowNginx;
#   3    install -WhatIf with the package as shipped (no config.json -> the app defaults) -> exit 0, the XML and
#        the steps are printed, nothing changed;
#   4    install -WhatIf -Config <TEST config.json below build\> -> exit 0; its XML is saved to the evidence folder
#        and checked by check-winsw-xml.ps1 (WinSW element names / rules + the real WinSW "status");
#   5    uninstall -WhatIf -> exit 0 "not installed - nothing to do".
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-076\tools\test-service-scripts-nonadmin.ps1 -Package dist\TaidaFlow-<...>
param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
. (Join-Path $root 'scripts\taidaflow-config.ps1')
if (-not [System.IO.Path]::IsPathRooted($Package)) { $Package = Join-Path $root $Package }
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
$evid = Split-Path -Parent $PSScriptRoot
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$install = Join-Path $Package 'scripts\install-nginx-service.ps1'
$uninstall = Join-Path $Package 'scripts\uninstall-nginx-service.ps1'
$script:fail = 0; $script:n = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    $script:n++; if (-not $ok) { $script:fail++ }
    [Console]::Out.WriteLine(("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' })))
}
function Run([string]$script, [string[]]$arguments, [string]$saveAs = '') {
    $ErrorActionPreference = 'Continue'
    $out = @(& $ps -NoProfile -ExecutionPolicy Bypass -File $script @arguments 2>&1 | ForEach-Object { "$_" })
    $rc = $LASTEXITCODE
    [Console]::Out.WriteLine("  > $(Split-Path -Leaf $script) $($arguments -join ' ')   (exit $rc)")
    foreach ($l in $out) { [Console]::Out.WriteLine("  | $l") }
    if ($saveAs) { [System.IO.File]::WriteAllLines((Join-Path $evid $saveAs), [string[]](@($out) + @("----", "EXIT=$rc")), (New-Object System.Text.UTF8Encoding($false))) }
    return [pscustomobject]@{ rc = $rc; out = $out }
}
$script:snapLines = @{}
function Snapshot([string]$d) {
    if (-not (Test-Path -LiteralPath $d)) { return '<does not exist>' }
    $lines = @(Get-ChildItem -LiteralPath $d -Recurse -Force | Sort-Object FullName | ForEach-Object { "$($_.FullName)|$(if ($_.PSIsContainer) { 'D' } else { $_.Length })|$($_.LastWriteTimeUtc.Ticks)" })
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $h = [BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes(($lines -join "`n")))).Replace('-', '') + " ($($lines.Count) entries)"
    if (-not $script:snapLines.ContainsKey($d)) { $script:snapLines[$d] = $lines }
    elseif ($h -ne $script:snapFirst[$d]) { Compare-Object $script:snapLines[$d] $lines | ForEach-Object { [Console]::Out.WriteLine("     changed in ${d}: $($_.SideIndicator) $($_.InputObject)") } }
    if (-not $script:snapFirst) { $script:snapFirst = @{} }
    if (-not $script:snapFirst.ContainsKey($d)) { $script:snapFirst[$d] = $h }
    return $h
}
function SvcCount { return @(Get-CimInstance Win32_Service -Filter "Name='TaidaFlowNginx'").Count }
[Console]::Out.WriteLine("=== w2-076 install/uninstall-nginx-service without administrator $((Get-Date).ToString('o')); admin=$(Test-TaidaFlowIsAdministrator); package $Package")
if (Test-TaidaFlowIsAdministrator) { [Console]::Out.WriteLine("this test must run WITHOUT administrator (it would install the service) - stopped"); exit 2 }
$pkgBefore = Snapshot $Package; $dataBefore = Snapshot 'C:\TaidaFlowData'; $svcBefore = SvcCount
[Console]::Out.WriteLine("before: package $pkgBefore ; C:\TaidaFlowData $dataBefore ; services TaidaFlowNginx: $svcBefore")

$r = Run $install @() 'd3-01-install-nonadmin.txt'
Check '1a install-nginx-service.ps1 without administrator: exit 5' ($r.rc -eq 5) "exit $($r.rc)"
Check '1b clear refusal text (needs an administrator, NOTHING was changed, how to run it / -WhatIf)' ((($r.out -join "`n") -match 'REFUSED: .*needs an administrator') -and (($r.out -join "`n") -match 'NOTHING was changed') -and (($r.out -join "`n") -match 'Run as administrator')) ''
Check '1c nothing changed: package folder identical, C:\TaidaFlowData identical, no service' ((Snapshot $Package) -eq $pkgBefore -and (Snapshot 'C:\TaidaFlowData') -eq $dataBefore -and (SvcCount) -eq 0) ''
$r = Run $uninstall @() 'd3-02-uninstall-nonadmin.txt'
Check '2a uninstall-nginx-service.ps1 without administrator: exit 5' ($r.rc -eq 5) "exit $($r.rc)"
Check '2b clear refusal text' ((($r.out -join "`n") -match 'REFUSED: .*needs an administrator') -and (($r.out -join "`n") -match 'NOTHING was changed')) ''
Check '2c nothing changed' ((Snapshot $Package) -eq $pkgBefore -and (Snapshot 'C:\TaidaFlowData') -eq $dataBefore -and (SvcCount) -eq 0) ''

$r = Run $install @('-WhatIf') 'd3-03-install-whatif-package-defaults.txt'
$t = $r.out -join "`n"
Check '3a install -WhatIf (package as shipped, app defaults): exit 0' ($r.rc -eq 0) "exit $($r.rc)"
Check '3b prints the XML and the steps, "-WhatIf: NOTHING changed"' ($t -match '\|\s+<service>' -and $t -match 'steps \(NOT executed: -WhatIf\)' -and $t -match '-WhatIf: NOTHING changed') ''
Check '3c nothing changed (no config.json / nginx.conf / nginx-service.xml, C:\TaidaFlowData untouched, no service)' ((Snapshot $Package) -eq $pkgBefore -and (Snapshot 'C:\TaidaFlowData') -eq $dataBefore -and (SvcCount) -eq 0) ''

# test config.json (below build\): data + logs in the test folder
$testDir = Join-Path $root 'build\w2-076-svc-test'
if (Test-Path $testDir) { Remove-Item -LiteralPath $testDir -Recurse -Force }
New-Item -ItemType Directory -Force $testDir | Out-Null
$testCfg = Join-Path $testDir 'config.json'
$w = Invoke-TaidaFlowWriteDefaultConfig (Join-Path $Package 'TaidaFlowApp.exe') $testCfg
$txt = [System.IO.File]::ReadAllText($testCfg)
$dataDir = Join-Path $testDir 'data'
$txt = [regex]::Replace($txt, '"dataDir": "[^"]*"', ('"dataDir": "' + ($dataDir -replace '\\', '\\').Replace('$', '$$') + '"'))
$txt = [regex]::Replace($txt, '"exe": "[^"]*"', ('"exe": "' + ((Join-Path $Package 'nginx\nginx.exe') -replace '\\', '\\').Replace('$', '$$') + '"'))
[System.IO.File]::WriteAllText($testCfg, $txt, (New-Object System.Text.UTF8Encoding($false)))
$r = Run $install @('-WhatIf', '-Config', $testCfg) 'd3-04-install-whatif-test-config.txt'
Check '4a install -WhatIf -Config <test config.json>: exit 0' ($r.rc -eq 0) "exit $($r.rc)"
$xmlLines = @($r.out | Where-Object { $_ -match '^  \| ' } | ForEach-Object { $_.Substring(4) })
$xmlFile = Join-Path $evid 'd3-05-nginx-service.whatif.xml'
[System.IO.File]::WriteAllText($xmlFile, (($xmlLines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
Check '4b XML extracted from the -WhatIf output' ($xmlLines.Count -gt 10 -and $xmlLines[0] -match '^<\?xml') "$($xmlLines.Count) line(s) -> $xmlFile"
Check '4c the XML uses the test config: logpath = <test>\data\logs, executable = <package>\nginx\nginx.exe' ((($xmlLines -join "`n") -match [regex]::Escape("<logpath>$dataDir\logs</logpath>")) -and (($xmlLines -join "`n") -match [regex]::Escape("<executable>$(Join-Path $Package 'nginx\nginx.exe')</executable>"))) ''
Check '4d nothing changed (package, C:\TaidaFlowData, no service; the test data folder was not created)' ((Snapshot $Package) -eq $pkgBefore -and (Snapshot 'C:\TaidaFlowData') -eq $dataBefore -and (SvcCount) -eq 0 -and -not (Test-Path $dataDir)) ''
$ErrorActionPreference = 'Continue'
$cx = @(& $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'check-winsw-xml.ps1') -Xml $xmlFile -WinSW (Join-Path $Package 'nginx\nginx-service.exe') -WorkDir (Join-Path $testDir 'winsw-parse') 2>&1 | ForEach-Object { "$_" })
$cxRc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
[System.IO.File]::WriteAllLines((Join-Path $evid 'd3-06-check-winsw-xml.txt'), [string[]](@($cx) + @("----", "EXIT=$cxRc")), (New-Object System.Text.UTF8Encoding($false)))
foreach ($l in $cx) { [Console]::Out.WriteLine("  | $l") }
Check '4e check-winsw-xml.ps1 (WinSW v2.12.0 element names / rules, real WinSW "status" = NonExistent): exit 0' ($cxRc -eq 0) "exit $cxRc"

$r = Run $uninstall @('-WhatIf') 'd3-07-uninstall-whatif.txt'
Check '5 uninstall -WhatIf: exit 0, "not installed - nothing to do"' ($r.rc -eq 0 -and ($r.out -join "`n") -match 'not installed - nothing to do') "exit $($r.rc)"
Remove-Item -LiteralPath $testDir -Recurse -Force
Check '6 at the end: package identical to the start, C:\TaidaFlowData identical, no service TaidaFlowNginx' ((Snapshot $Package) -eq $pkgBefore -and (Snapshot 'C:\TaidaFlowData') -eq $dataBefore -and (SvcCount) -eq 0) ''
[Console]::Out.WriteLine("=== $($script:n) check(s), $($script:fail) failed")
if ($script:fail) { exit 1 }
exit 0

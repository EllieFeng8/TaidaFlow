# w2-076 D2 evidence: what the package carries for the nginx service / Startup shortcut.
param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
if (-not [System.IO.Path]::IsPathRooted($Package)) { $Package = Join-Path $root $Package }
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
function Say([string]$m) { [Console]::Out.WriteLine($m) }
$bad = 0
Say "package: $Package"
Say "--- nginx\ (top level)"
Get-ChildItem -LiteralPath (Join-Path $Package 'nginx') | ForEach-Object { Say ("  {0,-22} {1}" -f $_.Name, $(if ($_.PSIsContainer) { '<dir>' } else { "$($_.Length) bytes  SHA-256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash)" })) }
Say "--- scripts\"
Get-ChildItem -LiteralPath (Join-Path $Package 'scripts') | ForEach-Object { Say ("  {0,-30} {1} bytes" -f $_.Name, $_.Length) }
$ex = Join-Path $Package 'nginx\nginx-service.exe'
$ref = 'C:\tools\winsw\WinSW-x64-2.12.0.exe'
$same = (Get-FileHash -LiteralPath $ex).Hash -eq (Get-FileHash -LiteralPath $ref).Hash
Say "nginx-service.exe identical to $ref : $same"; if (-not $same) { $bad++ }
$lic = (Get-FileHash -LiteralPath (Join-Path $Package 'nginx\LICENSE-WinSW.txt')).Hash -eq (Get-FileHash -LiteralPath 'C:\tools\winsw\LICENSE.txt').Hash
Say "LICENSE-WinSW.txt identical to C:\tools\winsw\LICENSE.txt : $lic"; if (-not $lic) { $bad++ }
$noXml = -not (Test-Path (Join-Path $Package 'nginx\nginx-service.xml'))
Say "nginx\nginx-service.xml NOT in the package : $noXml"; if (-not $noXml) { $bad++ }
Say "--- nginx\SOURCE-WinSW.txt"
Get-Content -LiteralPath (Join-Path $Package 'nginx\SOURCE-WinSW.txt') | ForEach-Object { Say "  $_" }
Say "--- VERSION.txt"
Get-Content -LiteralPath (Join-Path $Package 'VERSION.txt') | ForEach-Object { Say "  $_" }
Say "--- MANIFEST.txt lines of the w2-076 files"
$want = 'nginx\nginx-service.exe', 'nginx\LICENSE-WinSW.txt', 'nginx\SOURCE-WinSW.txt', 'scripts\install-nginx-service.ps1', 'scripts\uninstall-nginx-service.ps1',
        'scripts\add-startup-shortcut.ps1', 'scripts\remove-startup-shortcut.ps1', 'start-taidaflow.ps1', 'start-taidaflow.bat', 'stop-taidaflow.ps1', 'register-autostart.ps1', 'scripts\taidaflow-config.ps1', 'DEPLOY.md'
$man = [System.IO.File]::ReadAllLines((Join-Path $Package 'MANIFEST.txt'))
foreach ($w in $want) {
    $l = @($man | Where-Object { $_.StartsWith($w + "`t") })
    if ($l.Count -eq 1) { Say "  $($l[0])" } else { Say "  MISSING in MANIFEST: $w"; $bad++ }
}
$vt = [System.IO.File]::ReadAllText((Join-Path $Package 'VERSION.txt'))
$h = (Get-FileHash -LiteralPath $ex).Hash
if (-not $vt.Contains("WinSW 2.12.0") -or -not $vt.Contains($h)) { Say "VERSION.txt lacks WinSW version / SHA-256"; $bad++ } else { Say "VERSION.txt records WinSW 2.12.0 and SHA-256 $h" }
foreach ($f in 'start-taidaflow.ps1', 'stop-taidaflow.ps1', 'register-autostart.ps1', 'start-taidaflow.bat') {
    $s = (Get-FileHash -LiteralPath (Join-Path $Package $f)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $root "deploy\release\$f")).Hash
    Say "package $f = deploy\release\$f : $s"; if (-not $s) { $bad++ }
}
foreach ($f in 'install-nginx-service.ps1', 'uninstall-nginx-service.ps1', 'add-startup-shortcut.ps1', 'remove-startup-shortcut.ps1', 'taidaflow-config.ps1') {
    $s = (Get-FileHash -LiteralPath (Join-Path $Package "scripts\$f")).Hash -eq (Get-FileHash -LiteralPath (Join-Path $root "scripts\$f")).Hash
    Say "package scripts\$f = scripts\$f : $s"; if (-not $s) { $bad++ }
}
$d = (Get-FileHash -LiteralPath (Join-Path $Package 'DEPLOY.md')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $root 'docs\DEPLOY_AND_STARTUP.md')).Hash
Say "package DEPLOY.md = docs\DEPLOY_AND_STARTUP.md : $d"; if (-not $d) { $bad++ }
Say "problems: $bad"
if ($bad) { exit 1 }
exit 0

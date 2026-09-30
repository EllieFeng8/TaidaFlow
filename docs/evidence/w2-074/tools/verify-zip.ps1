# w2-074 D6: checks a package zip without touching the package folder.
#   1. SHA-256 of the zip (compare with the value in the w2-074 report)
#   2. entry list: one root folder = the package name, file count, separator style
#   3. extracts it twice below build\ (Expand-Archive, and the Windows Explorer engine Shell.Application
#      CopyHere = what "Extract All" / drag out of the zip uses) and runs check-manifest.ps1 on each copy
#      (every MANIFEST.txt entry same size + SHA-256, no extra file, no residue); the copies are deleted.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File verify-zip.ps1 -Zip <dist\TaidaFlow-....zip> [-Work build\w2-074-zipcheck]
# Exit: 0 all ok; 1 a check failed; 2 bad parameters.
param([Parameter(Mandatory = $true)][string]$Zip, [string]$Work = 'build\w2-074-zipcheck')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
$Zip = [System.IO.Path]::GetFullPath($Zip)
if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { [Console]::Out.WriteLine("no zip $Zip"); exit 2 }
$Work = [System.IO.Path]::GetFullPath((Join-Path $root $Work)).TrimEnd('\')
if (-not $Work.StartsWith($root + '\build\', [System.StringComparison]::OrdinalIgnoreCase)) { [Console]::Out.WriteLine("-Work must be below $root\build"); exit 2 }
$name = [System.IO.Path]::GetFileNameWithoutExtension($Zip)
$fails = 0
function Say([string]$m) { [Console]::Out.WriteLine($m) }
Say ("zip     : {0} ({1} bytes)" -f $Zip, (Get-Item -LiteralPath $Zip).Length)
Say ("SHA-256 : {0}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $Zip).Hash)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$za = [System.IO.Compression.ZipFile]::OpenRead($Zip)
try {
    $entries = @($za.Entries)
    $files = @($entries | Where-Object { $_.FullName -notmatch '[\\/]$' })
    $roots = @($entries | ForEach-Object { ($_.FullName -split '[\\/]')[0] } | Sort-Object -Unique)
    $bs = @($entries | Where-Object { $_.FullName.Contains('\') }).Count
    Say ("entries : {0} ({1} file(s)); root folder(s): {2}; entries with '\' separators: {3}" -f $entries.Count, $files.Count, ($roots -join ', '), $bs)
    if ($roots.Count -ne 1 -or $roots[0] -ne $name) { Say "FAIL root folder of the zip is not $name"; $fails++ }
} finally { $za.Dispose() }
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
New-Item -ItemType Directory -Force $Work | Out-Null
$check = Join-Path $PSScriptRoot 'check-manifest.ps1'
# 1. Expand-Archive
$a = Join-Path $Work 'expand-archive'
Expand-Archive -LiteralPath $Zip -DestinationPath $a
$out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check -Package (Join-Path $a $name)
$rc = $LASTEXITCODE
$out | ForEach-Object { Say "  expand-archive | $_" }
Say "Expand-Archive copy: check-manifest exit $rc"
if ($rc -ne 0) { $fails++ }
# 2. Windows Explorer engine (Shell.Application)
$b = Join-Path $Work 'explorer-shell'
New-Item -ItemType Directory -Force $b | Out-Null
$shell = New-Object -ComObject Shell.Application
$src = $shell.NameSpace($Zip)
$dst = $shell.NameSpace($b)
$dst.CopyHere($src.Items(), 4 + 16 + 512 + 1024)   # no progress UI, yes to all, no confirm mkdir, no error UI
$deadline = (Get-Date).AddMinutes(10)
$expected = $files.Count
do {
    Start-Sleep -Seconds 2
    $have = @(Get-ChildItem -LiteralPath $b -Recurse -File -Force -ErrorAction SilentlyContinue).Count
} while ($have -lt $expected -and (Get-Date) -lt $deadline)
Start-Sleep -Seconds 3
$out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $check -Package (Join-Path $b $name)
$rc = $LASTEXITCODE
$out | ForEach-Object { Say "  explorer-shell | $_" }
Say "Explorer (Shell.Application) copy: $have file(s), check-manifest exit $rc"
if ($rc -ne 0) { $fails++ }
Remove-Item -LiteralPath $Work -Recurse -Force
Say "extracted copies removed ($Work)"
Say "=== $fails check(s) failed"
if ($fails) { exit 1 }
exit 0

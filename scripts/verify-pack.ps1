# Verify the vendored wasm-mirror pack (integration-pack/wasm-mirror).
# (w2-062: PowerShell port of the former scripts/verify_pack.py; same checks, same output.)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-pack.ps1 [-Source <pack release dir>]
#
# 1. MANIFEST.sha256 check: every listed file exists and its SHA-256 matches (prints ok/total,
#    e.g. 24/24), and no file in the pack is missing from the manifest (MANIFEST.sha256 itself
#    excepted).
# 2. With -Source: the pack directory and the release source contain exactly the same file set
#    and every file is byte-identical (SHA-256 per file, each pair printed).
# 3. The legacy 1.0.0 extensionless `VERSION` file must not exist; VERSION.txt is reported.
#
# Exit 0 only when every check passes. Read-only: nothing is written.
param(
    [string]$Source = ""
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pack = Join-Path $root 'integration-pack\wasm-mirror'
function Say([string]$m) { [Console]::Out.WriteLine($m) }
function Get-Sha256([string]$path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }
# Relative path (forward slashes) -> full path of every file below $base (links not followed).
function Get-FilesOf([string]$base) {
    $full = [System.IO.Path]::GetFullPath($base).TrimEnd('\')
    $map = @{}
    foreach ($f in @(Get-ChildItem -LiteralPath $full -Recurse -File -Force)) {
        $map[$f.FullName.Substring($full.Length + 1).Replace('\', '/')] = $f.FullName
    }
    return $map
}
function Sorted([string[]]$items) { $a = [string[]]@($items); [System.Array]::Sort($a, [System.StringComparer]::Ordinal); return ,$a }
$status = 0

$manifest = Join-Path $pack 'MANIFEST.sha256'
$entries = New-Object System.Collections.Generic.List[object]
foreach ($line in [System.IO.File]::ReadAllLines($manifest, [System.Text.Encoding]::UTF8)) {
    if ($line.Trim() -ne '') {
        $parts = $line.Trim().Split([char[]]@(' ', "`t"), 2, [System.StringSplitOptions]::RemoveEmptyEntries)
        $entries.Add([pscustomobject]@{ digest = $parts[0].ToLowerInvariant(); name = $parts[1].Trim() })
    }
}
$ok = 0
foreach ($e in $entries) {
    $path = Join-Path $pack ($e.name -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Say "MANIFEST missing file: $($e.name)"
        continue
    }
    $actual = Get-Sha256 $path
    if ($actual -eq $e.digest) { $ok++ }
    else { Say "MANIFEST mismatch: $($e.name) expected=$($e.digest) actual=$actual" }
}
Say "manifest: $ok/$($entries.Count) ok"
if ($ok -ne $entries.Count -or $entries.Count -eq 0) { $status = 1 }
$listed = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
foreach ($e in $entries) { [void]$listed.Add($e.name) }
[void]$listed.Add('MANIFEST.sha256')
$packFiles = Get-FilesOf $pack
$unlisted = Sorted @($packFiles.Keys | Where-Object { -not $listed.Contains($_) })
foreach ($name in $unlisted) { Say "file not in MANIFEST: $name" }
if ($unlisted.Count -gt 0) { $status = 1 }

$versionTxtPath = Join-Path $pack 'VERSION.txt'
$versionTxt = if (Test-Path -LiteralPath $versionTxtPath -PathType Leaf) { ([System.IO.File]::ReadAllLines($versionTxtPath, [System.Text.Encoding]::UTF8))[0] } else { '<missing>' }
# Python set membership: exact (case-sensitive) name.
$legacy = [bool](@($packFiles.Keys | Where-Object { [string]::Equals($_, 'VERSION', [System.StringComparison]::Ordinal) }).Count)
Say "legacy_VERSION_present=$(if ($legacy) { 'True' } else { 'False' }) VERSION.txt=$versionTxt"
if ($legacy -or $versionTxt -eq '<missing>') { $status = 1 }

if ($Source -ne "") {
    $src = Get-FilesOf $Source
    $dst = $packFiles
    $onlySrc = Sorted @($src.Keys | Where-Object { -not $dst.ContainsKey($_) })
    $onlyDst = Sorted @($dst.Keys | Where-Object { -not $src.ContainsKey($_) })
    $same = 0
    foreach ($name in (Sorted @($src.Keys | Where-Object { $dst.ContainsKey($_) }))) {
        $a = Get-Sha256 $src[$name]; $b = Get-Sha256 $dst[$name]
        if ($a -eq $b) { $same++; Say "  same $a  $name" }
        else { Say "  DIFF $name source=$a pack=$b" }
    }
    foreach ($name in $onlySrc) { Say "only in source: $name" }
    foreach ($name in $onlyDst) { Say "only in pack: $name" }
    Say "source compare: identical=$same source_files=$($src.Count) pack_files=$($dst.Count) only_source=$($onlySrc.Count) only_pack=$($onlyDst.Count)"
    if ($onlySrc.Count -gt 0 -or $onlyDst.Count -gt 0 -or $same -ne $src.Count) { $status = 1 }
}
Say "verify_pack exit=$status"
exit $status

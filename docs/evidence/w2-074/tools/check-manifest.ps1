# w2-074: checks that a TaidaFlow package folder is exactly as shipped, from its own MANIFEST.txt.
#   * every MANIFEST line: file exists, same size, same SHA-256
#   * no file in the folder that MANIFEST does not list (MANIFEST.txt itself excepted)
#   * no run-time residue: config.json, config.effective.json, taidaflow-app.json, nginx\conf\nginx.conf*,
#     nginx\logs\, nginx\temp\, logs\, *.sqlite / *.db / *.log / *.ini / *.csv anywhere
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File check-manifest.ps1 -Package <folder>
# Exit: 0 all match and no residue; 1 mismatch / extra / missing / residue; 2 no MANIFEST.txt.
param([Parameter(Mandatory = $true)][string]$Package)
$ErrorActionPreference = 'Stop'
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
$m = Join-Path $Package 'MANIFEST.txt'
if (-not (Test-Path -LiteralPath $m -PathType Leaf)) { [Console]::Out.WriteLine("no MANIFEST.txt in $Package"); exit 2 }
$listed = @{}
$bad = 0; $ok = 0
foreach ($line in [System.IO.File]::ReadAllLines($m)) {
    if ($line.StartsWith('#') -or $line.Trim() -eq '') { continue }
    $p = $line.Split("`t")
    $rel = $p[0]; $len = [long]$p[1]; $sha = $p[2]
    $listed[$rel.ToLowerInvariant()] = $true
    $f = Join-Path $Package $rel
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { [Console]::Out.WriteLine("MISSING  $rel"); $bad++; continue }
    $fi = Get-Item -LiteralPath $f
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash
    if ($fi.Length -ne $len -or $h -ne $sha) { [Console]::Out.WriteLine("DIFFERS  $rel (size $($fi.Length) vs $len, sha $h vs $sha)"); $bad++ } else { $ok++ }
}
$extra = @(Get-ChildItem -LiteralPath $Package -Recurse -File -Force | Where-Object { $_.Name -ne 'MANIFEST.txt' -or $_.DirectoryName -ne $Package } |
           Where-Object { -not $listed.ContainsKey($_.FullName.Substring($Package.Length + 1).ToLowerInvariant()) })
foreach ($x in $extra) { [Console]::Out.WriteLine("EXTRA    $($x.FullName.Substring($Package.Length + 1))"); $bad++ }
$residue = @()
foreach ($r in 'config.json', 'config.effective.json', 'taidaflow-app.json', 'logs', 'nginx\logs', 'nginx\temp') {
    if (Test-Path -LiteralPath (Join-Path $Package $r)) { $residue += $r }
}
$residue += @(Get-ChildItem -LiteralPath (Join-Path $Package 'nginx\conf') -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'nginx.conf*' } | ForEach-Object { "nginx\conf\$($_.Name)" })
$residue += @(Get-ChildItem -LiteralPath $Package -Recurse -File -Force | Where-Object { $_.Extension -in '.sqlite', '.sqlite3', '.db', '.log', '.ini', '.csv' } | ForEach-Object { $_.FullName.Substring($Package.Length + 1) })
foreach ($r in $residue) { [Console]::Out.WriteLine("RESIDUE  $r"); $bad++ }
$all = @(Get-ChildItem -LiteralPath $Package -Recurse -File -Force)
[Console]::Out.WriteLine(("{0}: MANIFEST entries ok {1}, problems {2}; folder {3} file(s) {4:N0} bytes; residue: {5}" -f $Package, $ok, $bad, $all.Count,
    ($all | Measure-Object Length -Sum).Sum, $(if ($residue.Count) { $residue -join ', ' } else { 'none' })))
if ($bad) { exit 1 }
exit 0

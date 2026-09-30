# w2-076: moves every package folder / zip of dist\ into build\w2-076-retired-dist\ (git-ignored; NOT deleted),
# so that dist\ holds only the package built next. Refuses when a process runs from dist\ or a target exists.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$dist = Join-Path $root 'dist'
$ret = Join-Path $root 'build\w2-076-retired-dist'
function Say([string]$m) { [Console]::Out.WriteLine($m) }
Say "before: dist\"
Get-ChildItem -LiteralPath $dist | ForEach-Object { Say ("  {0}  {1}" -f $_.Name, $(if ($_.PSIsContainer) { "$(@(Get-ChildItem -LiteralPath $_.FullName -Recurse -File).Count) files" } else { "$($_.Length) bytes" })) }
$busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -and $_.Path.StartsWith($dist + '\', [System.StringComparison]::OrdinalIgnoreCase) } catch { $false } })
if ($busy.Count) { $busy | ForEach-Object { Say "running from dist: $($_.ProcessName) $($_.Id) $($_.Path)" }; exit 3 }
New-Item -ItemType Directory -Force $ret | Out-Null
foreach ($e in @(Get-ChildItem -LiteralPath $dist)) {
    $target = Join-Path $ret $e.Name
    if (Test-Path -LiteralPath $target) { Say "exists: $target - nothing moved"; exit 4 }
}
foreach ($e in @(Get-ChildItem -LiteralPath $dist)) {
    if (-not $e.PSIsContainer) { Say "  $($e.Name) SHA-256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $e.FullName).Hash)" }
    else { $v = Join-Path $e.FullName 'VERSION.txt'; if (Test-Path $v) { Get-Content -LiteralPath $v -TotalCount 3 | ForEach-Object { Say "  $($e.Name) | $_" } } }
    Move-Item -LiteralPath $e.FullName -Destination $ret
    Say "moved $($e.Name) -> build\w2-076-retired-dist\$($e.Name)"
}
Say "after: dist\ holds $(@(Get-ChildItem -LiteralPath $dist).Count) item(s); retired: $((Get-ChildItem -LiteralPath $ret | ForEach-Object { $_.Name }) -join ', ')"
exit 0

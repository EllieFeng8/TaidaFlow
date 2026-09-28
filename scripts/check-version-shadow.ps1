# Guard for the wasm-mirror `VERSION` / `<version>` shadowing landmine.
# (w2-062: PowerShell port of the former scripts/check_version_shadow.py; same checks, same output.)
#
# Pack 1.0.0 shipped a bare file named VERSION at the pack root. With the QDS default
# CMAKE_INCLUDE_CURRENT_DIR ON the pack root landed on the include path and, on the
# case-insensitive Windows filesystem, `#include <version>` resolved to that file instead of the
# C++ standard header. Pack 1.0.1 renamed it to VERSION.txt and turns CMAKE_INCLUDE_CURRENT_DIR
# off inside its own directory, so the host needs no workaround. This script proves it on
# already-built trees by scanning Ninja's recorded header dependencies (`ninja -t deps`):
#
#   * version_shadow_hits : object files depending on integration-pack/wasm-mirror/VERSION
#                           (must be 0)
#   * std_version_deps    : object files whose `version` dependency is some other file (the
#                           toolchain's standard header, e.g. MSVC STL / libc++)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-version-shadow.ps1 build\desktop build\wasm-release
#
# Ninja: TAIDAFLOW_QT_TOOLS\Ninja\ninja.exe (default C:\Qt\Tools\Ninja\ninja.exe), or -Ninja <path>.
# Exit 0 when no object file depends on the pack's VERSION file (and deps exist), 1 otherwise;
# 2 = no build folder given or ninja failed.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Ninja = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BuildDirs
)
$ErrorActionPreference = 'Stop'
if ($Ninja -eq "") {
    $tools = if ($env:TAIDAFLOW_QT_TOOLS) { $env:TAIDAFLOW_QT_TOOLS } else { 'C:\Qt\Tools' }
    $Ninja = Join-Path $tools 'Ninja\ninja.exe'
}
function Say([string]$m) { [Console]::Out.WriteLine($m) }
if (-not $BuildDirs -or $BuildDirs.Count -lt 1) {
    Say "usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-version-shadow.ps1 <build dir> [<build dir> ...]"
    exit 2
}
$pattern = New-Object System.Text.RegularExpressions.Regex ('integration-pack[/\\]wasm-mirror[/\\]version\s*$', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
$anyVersion = New-Object System.Text.RegularExpressions.Regex ('[/\\]version\s*$', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

$status = 0
foreach ($buildDir in $BuildDirs) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = @(& $Ninja -C $buildDir -t deps 2>$null | ForEach-Object { "$_" })
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($rc -ne 0) {
        Say "${buildDir}: ninja -t deps failed ($rc)"
        exit 2
    }
    $current = ''
    $hits = New-Object System.Collections.Generic.List[string]
    $objects = 0
    $stdDeps = @{}
    foreach ($line in $out) {
        if ($line.Length -gt 0 -and -not [char]::IsWhiteSpace($line[0])) {
            $current = $line.Split(':')[0]
            $objects++
            continue
        }
        $dep = $line.Trim()
        if ($pattern.IsMatch($dep)) { $hits.Add($current) }
        elseif ($anyVersion.IsMatch($dep)) {
            if ($stdDeps.ContainsKey($dep)) { $stdDeps[$dep]++ } else { $stdDeps[$dep] = 1 }
        }
    }
    $shown = ($buildDir -replace '/', '\').TrimEnd('\')
    $stdTotal = 0
    foreach ($v in $stdDeps.Values) { $stdTotal += $v }
    Say "${shown}: objects_with_deps=$objects version_shadow_hits=$($hits.Count) std_version_deps=$stdTotal"
    foreach ($h in $hits) { Say "  shadowed: $h" }
    $keys = [string[]]@($stdDeps.Keys)
    [System.Array]::Sort($keys, [System.StringComparer]::Ordinal)
    foreach ($k in $keys) { Say "  <version> -> $k ($($stdDeps[$k]) objects)" }
    if ($hits.Count -gt 0 -or $objects -eq 0) { $status = 1 }
}
exit $status

# Static DLL dependency check of a TaidaFlow field package (w2-057) - development PC tool.
#
# For every .exe / .dll in the package folder (recursive) it reads the import table with the MSVC
# "dumpbin /dependents" and checks that each imported DLL is found where Windows would load it
# for this application: the package folder itself (the exe folder; plugins are loaded into the
# same process), %SystemRoot%\System32, or an API set (api-ms-win-* / ext-ms-*, resolved by
# Windows). Delay-load imports are listed separately (optional at run time) and not required.
# A Qt install on PATH is NOT used, so a DLL that only exists in C:\Qt\... counts as missing.
# w2-076: a self-contained .NET single-file program (nginx\nginx-service.exe = WinSW v2.12.0, .NET 6) carries
# the .NET runtime INSIDE the exe (bundle marker with a header offset inside the file): its import table is all
# it needs from outside and is checked like any other PE (Windows DLLs + the UCRT api-ms-win-crt-* API sets of
# Windows 10/11). Such a file is labelled "self-contained .NET single file"; a .NET apphost that is NOT a
# bundle (it would need a .NET installation or runtime DLLs beside it) counts as missing ".NET runtime".
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-package-deps.ps1 -Package <folder> [-Dumpbin <dumpbin.exe>]
# Exit codes: 0 all imports resolved; 1 at least one import missing; 2 package / dumpbin not found.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Dumpbin = ""
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { [Console]::Out.WriteLine($m) }
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
if (-not (Test-Path (Join-Path $Package 'TaidaFlowApp.exe'))) { Say "no TaidaFlowApp.exe in $Package"; exit 2 }
if ($Dumpbin -eq "") {
    $vs = @(Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending)
    if ($vs.Count) { $Dumpbin = $vs[0].FullName }
}
if (-not $Dumpbin -or -not (Test-Path $Dumpbin)) { Say "dumpbin.exe not found (pass -Dumpbin)"; exit 2 }
Say "dumpbin: $Dumpbin"

$sys32 = Join-Path $env:SystemRoot 'System32'
$local = @{}
Get-ChildItem -LiteralPath $Package -File -Filter '*.dll' | ForEach-Object { $local[$_.Name.ToLowerInvariant()] = $true }
$pe = @(Get-ChildItem -LiteralPath $Package -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' } | Sort-Object FullName)
$missingTotal = 0
$delayed = @{}
# .NET single-file bundle marker (dotnet/runtime corehost bundle_marker: 8-byte header offset + SHA-256 of
# ".net core bundle"); the offset is 0 in a plain apphost and points inside the file in a bundle.
$bundleSig = [byte[]](0x8b, 0x12, 0x02, 0xb9, 0x6a, 0x61, 0x20, 0x38, 0x72, 0x7b, 0x93, 0x02, 0x14, 0xd7, 0xa0, 0x32,
                      0x13, 0xf5, 0xb9, 0xe6, 0xef, 0xae, 0x33, 0x18, 0xee, 0x3b, 0x2d, 0xce, 0x24, 0xb3, 0x6a, 0xae)
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$bundleSigText = $latin1.GetString($bundleSig)
function Get-DotNetKind([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text = $latin1.GetString($bytes)
    $i = $text.IndexOf($bundleSigText, [System.StringComparison]::Ordinal)
    if ($i -lt 8) {
        if ($text.IndexOf('hostfxr', [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -and $text.IndexOf('.NETCoreApp', [System.StringComparison]::Ordinal) -ge 0) { return 'apphost' }
        return ''
    }
    $offset = [BitConverter]::ToInt64($bytes, $i - 8)
    if ($offset -gt 0 -and $offset -lt $bytes.Length) {
        $rt = [regex]::Match($text, 'Microsoft\.NETCore\.App\.Runtime\.win-[a-z0-9]+/([0-9.]+)')
        return "bundle$(if ($rt.Success) { " .NET $($rt.Groups[1].Value)" })"
    }
    return 'apphost'
}
$ErrorActionPreference = 'Continue'
foreach ($f in $pe) {
    $out = & $Dumpbin /nologo /dependents $f.FullName 2>&1 | ForEach-Object { "$_" }
    $section = ''
    $deps = @(); $delay = @()
    foreach ($line in $out) {
        if ($line -match 'has the following dependencies') { $section = 'imp'; continue }
        if ($line -match 'has the following delay load dependencies') { $section = 'delay'; continue }
        if ($line -match '^\s+Summary') { $section = ''; continue }
        if ($section -and $line -match '^\s+(\S+\.dll)\s*$') {
            if ($section -eq 'imp') { $deps += $Matches[1] } else { $delay += $Matches[1] }
        }
    }
    $rel = $f.FullName.Substring($Package.Length + 1)
    $missing = @()
    foreach ($d in $deps) {
        $n = $d.ToLowerInvariant()
        if ($local.ContainsKey($n)) { continue }
        if ($n -like 'api-ms-win-*' -or $n -like 'ext-ms-*') { continue }
        if (Test-Path (Join-Path $sys32 $d)) { continue }
        $missing += $d
    }
    foreach ($d in $delay) { $delayed[$d.ToLowerInvariant()] = $true }
    $netNote = ''
    if ($f.Extension -eq '.exe') {
        $kind = Get-DotNetKind $f.FullName
        if ($kind -like 'bundle*') { $netNote = "; self-contained .NET single file ($($kind.Substring(6).Trim()), runtime inside the exe)" }
        elseif ($kind -eq 'apphost') { $missing += '.NET runtime (framework-dependent .NET apphost, not a self-contained single file)' }
    }
    if ($missing.Count) {
        Say ("MISSING  {0}: {1}" -f $rel, ($missing -join ', '))
        $missingTotal += $missing.Count
    } else {
        Say ("ok       {0} ({1} import(s){2}{3})" -f $rel, $deps.Count, $(if ($delay.Count) { ", delay-load: " + ($delay -join ', ') } else { '' }), $netNote)
    }
}
$ErrorActionPreference = 'Stop'
$notFoundDelay = @($delayed.Keys | Where-Object { -not $local.ContainsKey($_) -and -not (Test-Path (Join-Path $sys32 $_)) -and $_ -notlike 'api-ms-win-*' -and $_ -notlike 'ext-ms-*' })
Say ""
Say ("checked {0} PE file(s); missing imports: {1}; delay-load DLLs not present (optional): {2}" -f $pe.Count, $missingTotal,
     $(if ($notFoundDelay.Count) { $notFoundDelay -join ', ' } else { 'none' }))
if ($missingTotal -gt 0) { exit 1 }
exit 0

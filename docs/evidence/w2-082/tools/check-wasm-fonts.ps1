# w2-082 D3 (web, no browser): does the WebAssembly build contain the current embedded fonts and
# the UI font code of App/main.cpp?
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-082\tools\check-wasm-fonts.ps1
#            [-Wasm build\wasm-release\TaidaFlowApp.wasm] [-CompareRev HEAD] [-OutFile docs\evidence\w2-082\d3-wasm-artifact-check.txt]
# 1. The 'head' table of App\fonts\TaidaFlowNotoSansTC-{Regular,Bold}.ttf (54 bytes; it holds the
#    file's checksum adjustment and dates, so it differs for every generated file) must be found in the
#    .wasm data. Whole files are not searched: the Emscripten optimizer splits data segments at runs of
#    zero bytes (tables with zeros, e.g. maxp / OS/2 / cmap, are split). The same check on the fonts of
#    git revision -CompareRev (the previous subset) must NOT match when the fonts were regenerated.
# 2. The log strings of the UI font code (main.cpp applyUiFont, embeddedfonts.cpp) and "DejaVu Sans Mono"
#    (Consolas chain on the web) must be in the .wasm; UTF-16 "TaidaFlow Noto Sans TC" is not required
#    (the family name comes from the font file at run time).
# Exit 0 = current fonts found and code strings present; 1 = otherwise.
param(
    [string]$Wasm = "build\wasm-release\TaidaFlowApp.wasm",
    [string]$CompareRev = "HEAD",
    [string]$OutFile = "docs\evidence\w2-082\d3-wasm-artifact-check.txt"
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
$latin1 = [Text.Encoding]::GetEncoding(28591)
$wasmPath = Join-Path $root $Wasm
$w = $latin1.GetString([IO.File]::ReadAllBytes($wasmPath))
$out = New-Object System.Collections.Generic.List[string]
$out.Add("$Wasm : $($w.Length) bytes, $((Get-Item $wasmPath).LastWriteTime.ToString('s'))")
$failed = 0

function U32([byte[]]$b, [int]$o) { ([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o + 1] -shl 16) -bor ([uint32]$b[$o + 2] -shl 8) -bor [uint32]$b[$o + 3] }
function HeadFound([byte[]]$b) {
    $num = ([int]$b[4] -shl 8) -bor $b[5]
    for ($i = 0; $i -lt $num; $i++) {
        $r = 12 + 16 * $i
        if ($latin1.GetString($b, $r, 4) -ne 'head') { continue }
        $seg = $latin1.GetString($b, (U32 $b ($r + 8)), (U32 $b ($r + 12)))
        return $w.IndexOf($seg, [StringComparison]::Ordinal)
    }
    return -2
}

foreach ($f in 'TaidaFlowNotoSansTC-Regular.ttf', 'TaidaFlowNotoSansTC-Bold.ttf') {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root "App\fonts\$f"))
    $hash = (Get-FileHash (Join-Path $root "App\fonts\$f") -Algorithm SHA256).Hash.Substring(0, 16)
    $at = HeadFound $bytes
    $ok = $at -ge 0
    if (-not $ok) { $failed++ }
    $out.Add(("{0} App\fonts\{1} ({2} bytes, sha256 {3}...): head table in .wasm at {4}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $f, $bytes.Length, $hash, $at))
    if ($CompareRev) {
        $tmp = [IO.Path]::GetTempFileName()
        & cmd /c "git show ${CompareRev}:App/fonts/$f > `"$tmp`""
        $old = [IO.File]::ReadAllBytes($tmp)
        [IO.File]::Delete($tmp)
        $oldHash = if ($old.Length) { [BitConverter]::ToString((New-Object Security.Cryptography.SHA256Managed).ComputeHash($old)).Replace('-', '').Substring(0, 16) } else { '' }
        if ($old.Length -lt 12) { $out.Add("     ${CompareRev}:App/fonts/$f not readable") }
        elseif ($oldHash -eq $hash) { $out.Add("     ${CompareRev}:App/fonts/$f is the same file (not regenerated)") }
        else {
            $oldAt = HeadFound $old
            $out.Add(("     {0}:App/fonts/{1} ({2} bytes, sha256 {3}...): head table in .wasm at {4} (expected -1: previous subset not in this build)" -f $CompareRev, $f, $old.Length, $oldHash, $oldAt))
            if ($oldAt -ge 0) { $failed++ }
        }
    }
}
foreach ($p in '[UiFont] interface font:', 'is not available; keeping the default font', 'Embedded CJK font loaded:',
               'CJK fallback chain:', 'DejaVu Sans Mono') {
    $n = ([regex]::Matches($w, [regex]::Escape($p))).Count
    if ($n -lt 1) { $failed++ }
    $out.Add(("{0} '{1}' hits={2}" -f $(if ($n -ge 1) { 'PASS' } else { 'FAIL' }), $p, $n))
}
$out.Add("result: $(if ($failed) { "$failed check(s) FAILED" } else { 'all checks passed' })")
[IO.File]::WriteAllLines((Join-Path $root $OutFile), [string[]]$out, (New-Object System.Text.UTF8Encoding($false)))
$out | ForEach-Object { [Console]::Out.WriteLine($_) }
exit $(if ($failed) { 1 } else { 0 })

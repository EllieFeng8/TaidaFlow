# w2-077 D2: tests of scripts\check-wasm-backend.ps1 (F1: Debug build folders) on REAL build trees.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-077\tools\test-check-wasm-backend.ps1
#       [-DebugDir cmake-build-debug-visual-studio] [-ReleaseDir build\w2-077-desktop-release]
#       [-WasmDir build\w2-077-wasm-release] [-Ninja <ninja.exe>] [-Work build\w2-077-cwb-test]
#
# 1. Positive: the unmodified Debug, Release and WASM trees -> exit 0 (Debug alone, Release + WASM,
#    Debug + WASM).
# 2. Unchanged verdict: the previous version of the script (git show HEAD:scripts/check-wasm-backend.ps1)
#    and the new one print the same lines for the Release + WASM trees; on the Debug tree the old one
#    says FAIL (the F1 bug) and the new one OK.
# 3. Negative (mutation tests): the graph/cache/binary files of the real trees are copied into
#    <Work>\<case> (build.ninja, CMakeFiles\rules.ninja, CMakeCache.txt, TaidaFlowApp.exe or
#    TaidaFlowApp.js + .wasm) and ONE thing is changed per case; the checker must then say FAIL:
#      copy-debug          unchanged copy of the Debug tree            -> OK (the copy method is valid)
#      copy-wasm           unchanged copy of the WASM tree             -> OK
#      debug-no-qt-libs    Qt6<backend>d.lib removed from build.ninja  -> FAIL, backend_qt_libs (0)
#      debug-no-core-cpp   Core\core.cpp renamed in build.ninja        -> FAIL, backend_sources (8)
#      release-no-qt-libs  Qt6<backend>.lib removed from build.ninja   -> FAIL, backend_qt_libs (0)
#      wasm-plus-lib-a     libQt6SerialBus.a added to the link line    -> FAIL, backend_qt_libs (1)
#      wasm-plus-lib-d     Qt6HttpServerd.lib added to the link line   -> FAIL, backend_qt_libs (1)
#      wasm-plus-source    a Core\SqlManager.cpp compile edge added    -> FAIL, backend_sources (1)
# Exit 0 = every expectation met; 1 = at least one not met; 2 = a tree is missing.
[CmdletBinding()]
param(
    [string]$DebugDir = 'cmake-build-debug-visual-studio',
    [string]$ReleaseDir = 'build\w2-077-desktop-release',
    [string]$WasmDir = 'build\w2-077-wasm-release',
    [string]$Ninja = '',
    [string]$Work = 'build\w2-077-cwb-test'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
$checker = Join-Path $root 'scripts\check-wasm-backend.ps1'
function Say([string]$m) { [Console]::Out.WriteLine($m) }
foreach ($d in @($DebugDir, $ReleaseDir, $WasmDir)) {
    if (-not (Test-Path (Join-Path $d 'build.ninja'))) { Say "missing build tree: $d"; exit 2 }
}
if (-not $Work.StartsWith('build\', [System.StringComparison]::OrdinalIgnoreCase)) { Say "-Work must be below build\"; exit 2 }
$workFull = Join-Path $root $Work
if (Test-Path $workFull) { Remove-Item -LiteralPath $workFull -Recurse -Force }
New-Item -ItemType Directory -Path $workFull | Out-Null

$failures = 0
function Run-Checker([string]$script, [string[]]$dirs) {
    $ErrorActionPreference = 'Continue'
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script)
    if ($Ninja -ne '') { $a += @('-Ninja', $Ninja) }
    $a += $dirs
    $out = @(& powershell.exe @a 2>&1 | ForEach-Object { "$_" })
    return @{ rc = $LASTEXITCODE; out = $out }
}
function Expect([string]$name, $r, [int]$rc, [string]$mustContain) {
    $ok = ($r.rc -eq $rc)
    if ($ok -and $mustContain -ne '') { $ok = @($r.out | Where-Object { $_.Contains($mustContain) }).Count -gt 0 }
    Say ("[{0}] {1}: exit={2} (expected {3}{4})" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $r.rc, $rc,
         $(if ($mustContain -ne '') { ", output has '$mustContain'" } else { '' }))
    foreach ($l in $r.out) { Say "    $l" }
    if (-not $ok) { $script:failures++ }
}

Say "== 1. positive: real trees =="
Expect 'debug' (Run-Checker $checker @($DebugDir)) 0 'verdict: OK'
Expect 'release+wasm' (Run-Checker $checker @($ReleaseDir, $WasmDir)) 0 'check_wasm_backend exit=0'
Expect 'debug+wasm' (Run-Checker $checker @($DebugDir, $WasmDir)) 0 'check_wasm_backend exit=0'

Say "== 2. old (HEAD) vs new script =="
$old = Join-Path $workFull 'old-check-wasm-backend.ps1'
$oldText = (& git -C $root show HEAD:scripts/check-wasm-backend.ps1) -join "`r`n"
[System.IO.File]::WriteAllText($old, $oldText + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
$rOld = Run-Checker $old @($ReleaseDir, $WasmDir)
$rNew = Run-Checker $checker @($ReleaseDir, $WasmDir)
$same = ($rOld.rc -eq $rNew.rc) -and (($rOld.out -join "`n") -ceq ($rNew.out -join "`n"))
Say ("[{0}] release+wasm: old exit={1}, new exit={2}, output identical={3} ({4} lines)" -f $(if ($same) { 'PASS' } else { 'FAIL' }), $rOld.rc, $rNew.rc, $same, $rNew.out.Count)
if (-not $same) { $failures++; Say '  old:'; $rOld.out | ForEach-Object { Say "    $_" }; Say '  new:'; $rNew.out | ForEach-Object { Say "    $_" } }
Expect 'debug with the OLD script (F1 bug reproduced)' (Run-Checker $old @($DebugDir)) 1 'backend_qt_libs (0)'

Say "== 3. mutation tests (copies under $Work) =="
function Copy-Tree([string]$from, [string]$case) {
    $dst = Join-Path $workFull $case
    New-Item -ItemType Directory -Path (Join-Path $dst 'CMakeFiles') -Force | Out-Null
    foreach ($f in @('build.ninja', 'CMakeCache.txt', 'CMakeFiles\rules.ninja', 'TaidaFlowApp.exe', 'TaidaFlowApp.js', 'TaidaFlowApp.wasm')) {
        $s = Join-Path $from $f
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s -Destination (Join-Path $dst $f) }
    }
    return $dst
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
function Edit-Ninja([string]$dir, [scriptblock]$edit, [string]$what) {
    $p = Join-Path $dir 'build.ninja'
    $t = [System.IO.File]::ReadAllText($p, $utf8)
    $n = & $edit $t
    if ($n -ceq $t) { Say "    (mutation '$what' changed nothing!)"; $script:failures++ }
    [System.IO.File]::WriteAllText($p, $n, $utf8)
}
$backendLib = '(?i)Qt6(SerialBus|SerialPort|Sql|HttpServer|Concurrent)d?\.lib'

$d = Copy-Tree $DebugDir 'copy-debug'
Expect 'copy-debug (control)' (Run-Checker $checker @($d)) 0 'verdict: OK'
$d = Copy-Tree $WasmDir 'copy-wasm'
Expect 'copy-wasm (control)' (Run-Checker $checker @($d)) 0 'verdict: OK'

$d = Copy-Tree $DebugDir 'debug-no-qt-libs'
Edit-Ninja $d { param($t) [regex]::Replace($t, $backendLib, 'removed.lib') } 'remove Qt6<backend>d.lib'
Expect 'debug-no-qt-libs' (Run-Checker $checker @($d)) 1 'backend_qt_libs (0)'

$d = Copy-Tree $DebugDir 'debug-no-core-cpp'
Edit-Ninja $d { param($t) [regex]::Replace($t, '([/\\]Core[/\\])core\.cpp(?=[\s"]|$)', '${1}core_removed.cpp', 'Multiline') } 'rename Core\core.cpp'
Expect 'debug-no-core-cpp' (Run-Checker $checker @($d)) 1 'backend_sources (8)'

$d = Copy-Tree $ReleaseDir 'release-no-qt-libs'
Edit-Ninja $d { param($t) [regex]::Replace($t, $backendLib, 'removed.lib') } 'remove Qt6<backend>.lib'
Expect 'release-no-qt-libs' (Run-Checker $checker @($d)) 1 'backend_qt_libs (0)'

# The link edge of TaidaFlowApp.js: its first "  LINK_LIBRARIES = " line.
function Add-WasmLib([string]$dir, [string]$lib) {
    Edit-Ninja $dir {
        param($t)
        $m = [regex]::Match($t, '^build TaidaFlowApp\.js[ :].*?^  LINK_LIBRARIES = ', 'Multiline, Singleline')
        if (-not $m.Success) { return $t }
        $t.Insert($m.Index + $m.Length, "$lib ")
    } "add $lib"
}
$d = Copy-Tree $WasmDir 'wasm-plus-lib-a'
Add-WasmLib $d 'C:/Qt/6.8.3/wasm_singlethread/lib/libQt6SerialBus.a'
Expect 'wasm-plus-lib-a' (Run-Checker $checker @($d)) 1 'backend_qt_libs (1): SerialBus'

$d = Copy-Tree $WasmDir 'wasm-plus-lib-d'
Add-WasmLib $d 'C:/Qt/6.8.3/msvc2022_64/lib/Qt6HttpServerd.lib'
Expect 'wasm-plus-lib-d' (Run-Checker $checker @($d)) 1 'backend_qt_libs (1): HttpServer'

# A compile edge for Core/SqlManager.cpp whose object file is an input of TaidaFlowApp.js.
$d = Copy-Tree $WasmDir 'wasm-plus-source'
Edit-Ninja $d {
    param($t)
    $m = [regex]::Match($t, '^build TaidaFlowApp\.js: (\S+) ', 'Multiline')
    if (-not $m.Success) { return $t }
    $rule = $m.Groups[1].Value
    $t2 = $t.Insert($m.Index + $m.Length, 'CMakeFiles/w2077_probe/SqlManager.cpp.o ')
    # ninja -t commands lists commands of non-phony edges only: use a real rule with a command.
    $edge = "rule w2077_cc`n  command = em++ -c $($root -replace '\\','/')/Core/SqlManager.cpp -o `$out`n" +
            "build CMakeFiles/w2077_probe/SqlManager.cpp.o: w2077_cc`n"
    $edge + $t2
} 'add a Core\SqlManager.cpp compile edge'
Expect 'wasm-plus-source' (Run-Checker $checker @($d)) 1 'backend_sources (1): SqlManager.cpp'

Say ''
Say "test-check-wasm-backend: $failures expectation(s) not met"
exit $(if ($failures -eq 0) { 0 } else { 1 })

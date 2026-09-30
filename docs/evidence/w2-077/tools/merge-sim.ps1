# w2-077 D3: does the F2 block in the root CMakeLists.txt add merge conflicts when main is merged into core?
# Three-way merges with `git merge-file` (no repository write; temp files under build\w2-077-merge-sim):
#   base   = main:CMakeLists.txt (main is fully merged into core: merge-base(main, core) = main)
#   ours   = HEAD:CMakeLists.txt (core without the block)  vs  the working-tree file (core with the block)
#   theirs = main + one hypothetical future main edit (t0..t4)
# Prints the conflict count of each merge; exit 0 when the block never adds a conflict.
#   powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-077\tools\merge-sim.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$w = Join-Path $root 'build\w2-077-merge-sim'
if (Test-Path $w) { Remove-Item -LiteralPath $w -Recurse -Force }
New-Item -ItemType Directory -Path $w | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
function Lf([string]$t) { ($t -replace "`r`n", "`n") }
function Put([string]$name, [string]$text) { [System.IO.File]::WriteAllText((Join-Path $w $name), (Lf $text), $utf8) }
function GitShow([string]$spec) { $o = & git -C $root show $spec; if ($LASTEXITCODE -ne 0) { throw "git show $spec failed" }; ($o -join "`n") + "`n" }
$base = GitShow 'main:CMakeLists.txt'
Put 'base.txt' $base
Put 'ours_old.txt' (GitShow 'HEAD:CMakeLists.txt')
Put 'ours_new.txt' ([System.IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt'), $utf8))
$variants = [ordered]@{
    't0 main unchanged'                                  = $base
    't1 main adds a component to find_package(Qt6 ...)'  = $base.Replace("    Network WebSockets`n", "    Network WebSockets Svg`n")
    't2 main raises cmake_minimum_required'              = $base.Replace('cmake_minimum_required(VERSION 3.21.1)', 'cmake_minimum_required(VERSION 3.22)')
    't3 main adds a line after the QML_IMPORT_PATH block' = $base.Replace("    FORCE`n)`n", "    FORCE`n)`nset(MAIN_EXTRA ON)`n")
    't4 main edits qt_standard_project_setup()'          = $base.Replace("qt_standard_project_setup()`n", "qt_standard_project_setup(REQUIRES 6.8)`n")
}
$bad = 0
foreach ($k in $variants.Keys) {
    if ($k -ne 't0 main unchanged' -and $variants[$k] -ceq $base) { Write-Output "$k : variant did not change main (test broken)"; $bad++; continue }
    Put 'theirs.txt' $variants[$k]
    $r = @{}
    foreach ($o in 'ours_old', 'ours_new') {
        Copy-Item -LiteralPath (Join-Path $w "$o.txt") -Destination (Join-Path $w 'merge.txt') -Force
        & git merge-file -q (Join-Path $w 'merge.txt') (Join-Path $w 'base.txt') (Join-Path $w 'theirs.txt')
        $r[$o] = $LASTEXITCODE
    }
    $same = ($r['ours_old'] -eq $r['ours_new'])
    if (-not $same) { $bad++ }
    Write-Output ("{0}: conflicts without the w2-077 block={1}, with the block={2} -> {3}" -f $k, $r['ours_old'], $r['ours_new'], $(if ($same) { 'no new conflict' } else { 'NEW CONFLICT' }))
}
Write-Output "merge-sim: $bad case(s) where the block adds a conflict"
exit $(if ($bad -eq 0) { 0 } else { 1 })

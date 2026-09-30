# w2-082 D2: which non-ASCII characters can appear on screen, and are they all in the embedded
# font subset (App\fonts\charset.txt)?
#
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-082\tools\scan-ui-symbols.ps1
#            [-OutFile docs\evidence\w2-082\d2-ui-symbols.txt]
# Scans the STRING LITERALS ("..." and, in QML / JS, '...') of the UI sources - TaidaFlowContent,
# TaidaFlow (*.qml, *.js) and the C++ of App and Core without tests (*.cpp, *.h) - skipping whole-line
# comments. Every non-ASCII character is listed with its code point, count and first location; the
# non-Han ones (symbols, punctuation, Latin-1) separately. Then each of them and printable ASCII
# (U+0020..U+007E) is looked up in App\fonts\charset.txt.
# Exit 0 = all covered; 1 = at least one character is missing from charset.txt.
param([string]$OutFile = "docs\evidence\w2-082\d2-ui-symbols.txt")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
$utf8 = New-Object System.Text.UTF8Encoding($false)
$charset = [IO.File]::ReadAllText((Join-Path $root 'App\fonts\charset.txt'), $utf8).TrimEnd("`r", "`n")
$inCharset = @{}
foreach ($c in $charset.ToCharArray()) { $inCharset[[int]$c] = $true }

$files = @()
foreach ($d in 'TaidaFlowContent', 'TaidaFlow') {
    $files += Get-ChildItem -Recurse -File (Join-Path $root $d) -Include *.qml, *.js
}
foreach ($d in 'App', 'Core') {
    $files += Get-ChildItem -Recurse -File (Join-Path $root $d) -Include *.cpp, *.h |
        Where-Object { $_.FullName -notmatch '\\tests\\|\\autogen\\' }
}
$dq = [regex]'"(?:[^"\\]|\\.)*"'
$sq = [regex]"'(?:[^'\\]|\\.)*'"
$found = @{}   # code point -> @{count; first}
foreach ($f in $files) {
    $isQml = $f.Extension -in '.qml', '.js'
    $lines = [IO.File]::ReadAllLines($f.FullName, $utf8)
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        $t = $line.TrimStart()
        if ($t.StartsWith('//') -or $t.StartsWith('*') -or $t.StartsWith('/*')) { continue }
        $lits = @($dq.Matches($line) | ForEach-Object { $_.Value })
        if ($isQml) { $lits += @($sq.Matches($line) | ForEach-Object { $_.Value }) }
        foreach ($lit in $lits) {
            foreach ($ch in $lit.ToCharArray()) {
                $cp = [int]$ch
                if ($cp -le 0x7E) { continue }
                if (-not $found.ContainsKey($cp)) {
                    $rel = $f.FullName.Substring($root.Length + 1)
                    $found[$cp] = @{ count = 0; first = "${rel}:$($i + 1)" }
                }
                $found[$cp].count++
            }
        }
    }
}

$out = New-Object System.Collections.Generic.List[string]
$out.Add("w2-082 scan of UI string literals: $($files.Count) files (TaidaFlowContent, TaidaFlow *.qml/*.js; App, Core *.cpp/*.h without tests)")
$out.Add("charset.txt: $($charset.Length) characters")
$han = @($found.Keys | Where-Object { $_ -ge 0x3400 -and $_ -le 0x9FFF } | Sort-Object)
$sym = @($found.Keys | Where-Object { -not ($_ -ge 0x3400 -and $_ -le 0x9FFF) } | Sort-Object)
$missing = New-Object System.Collections.Generic.List[string]
$out.Add("")
$out.Add("== non-Han characters in UI string literals: $($sym.Count) (code point, char, count, first location, in charset.txt)")
foreach ($cp in $sym) {
    $ok = $inCharset.ContainsKey($cp)
    if (-not $ok) { $missing.Add(('U+{0:X4}' -f $cp)) }
    $out.Add(('U+{0:X4} {1}  x{2,-4} {3,-55} {4}' -f $cp, [char]$cp, $found[$cp].count, $found[$cp].first, $(if ($ok) { 'yes' } else { 'MISSING' })))
}
$hanMissing = @($han | Where-Object { -not $inCharset.ContainsKey($_) })
foreach ($cp in $hanMissing) { $missing.Add(('U+{0:X4}' -f $cp)) }
$out.Add("")
$out.Add("== Han characters in UI string literals: $($han.Count), missing from charset.txt: $($hanMissing.Count)")
$asciiMissing = @(0x20..0x7E | Where-Object { -not $inCharset.ContainsKey($_) })
foreach ($cp in $asciiMissing) { $missing.Add(('U+{0:X4}' -f $cp)) }
$out.Add("== printable ASCII U+0020..U+007E: 95, missing from charset.txt: $($asciiMissing.Count)")
$out.Add("")
$out.Add("result: $($missing.Count) missing $($missing -join ' ')")
[IO.File]::WriteAllLines((Join-Path $root $OutFile), [string[]]$out, $utf8)
$out | ForEach-Object { [Console]::Out.WriteLine($_) }
exit $(if ($missing.Count) { 1 } else { 0 })

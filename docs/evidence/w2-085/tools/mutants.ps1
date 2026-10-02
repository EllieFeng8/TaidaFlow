# w2-085 D4 (negative controls): tst_limit_alarms must FAIL for a deliberately broken LimitAlarms.cpp.
# Each mutant is a copy of Core\ and TaidaFlowContent\ (tar, nothing in the real folders is changed) under
# build\w2-085-run\mutants\<name>, with ONE text change in Core\LimitAlarms.cpp; only the target
# tst_limit_alarms is built (MSVC 2022 x64, Qt 6.8.3, Release) and run.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-085\tools\mutants.ps1 [-OutDir docs\evidence\w2-085]
# Exit 0 = the unchanged copy passes and every mutant is caught (test exit code != 0).
param([string]$OutDir = 'docs\evidence\w2-085')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$report = Join-Path $OutDir 'd4-mutants.txt'
Set-Content -Encoding utf8 $report "=== w2-085 negative controls $(Get-Date -Format o)"
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Write-Host $line; Add-Content -Encoding utf8 $report $line }
$base = Join-Path $root 'build\w2-085-run\mutants'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$mutants = [ordered]@{
    'original'          = @('', '')
    'M1-equal-is-alarm' = @('&& value > entry.value(QStringLiteral("upper")).toDouble();', '&& value >= entry.value(QStringLiteral("upper")).toDouble();')
    'M2-no-2s-wait'     = @('if (t - side.normalSinceMs < m_options.resolveAfterMs)', 'if (false)')
    'M3-offset-ignored' = @('*out = raw + settings.value(k).toMap().value(QStringLiteral("offset")).toDouble();', '*out = raw;')
    'M4-reviolation-keeps-countdown' = @(".arg(side.row.id);`n                side.normalSinceMs = -1;", '.arg(side.row.id);')
    'M5-no-restart-takeover' = @('if (!check.restartChecked && !takeOverRestart(check, value, above, below))', 'if (false)')
    'M6-duplicates'     = @("        if (side.active) {`n            if (side.normalSinceMs >= 0) {", "        if (false) {`n            if (side.normalSinceMs >= 0) {")
}
$failed = 0
foreach ($name in $mutants.Keys) {
    $dir = Join-Path $base $name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force $dir | Out-Null
    # cmd's pipe keeps the archive bytes (a PowerShell 5.1 pipe between native programs would not).
    & cmd /c "tar -cf - Core TaidaFlowContent | tar -xf - -C `"$dir`""
    $src = Join-Path $dir 'Core\LimitAlarms.cpp'
    $text = [System.IO.File]::ReadAllText($src).Replace("`r`n", "`n")
    $from = $mutants[$name][0]; $to = $mutants[$name][1]
    if ($from -ne '') {
        $count = ([regex]::Matches($text, [regex]::Escape($from))).Count
        if ($count -ne 1) { Say "$name : pattern found $count times (expected 1) - script error"; $failed++; continue }
        $text = $text.Replace($from, $to)
        [System.IO.File]::WriteAllText($src, $text, (New-Object System.Text.UTF8Encoding($false)))
    }
    $log = Join-Path $dir 'build-and-run.log'
    $bat = Join-Path $dir 'run.bat'
    Set-Content -Encoding ascii $bat @(
        '@echo off',
        "call `"$vcvars`" >nul || exit /b 90",
        'set "PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH%"',
        "C:\Qt\Tools\CMake_64\bin\cmake.exe -S `"$dir\Core\tests`" -B `"$dir\build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 >nul || exit /b 91",
        "C:\Qt\Tools\CMake_64\bin\cmake.exe --build `"$dir\build`" --target tst_limit_alarms || exit /b 92",
        "set QT_QPA_PLATFORM=offscreen",
        "`"$dir\build\tst_limit_alarms.exe`" -o `"$dir\result.txt,txt`"",
        'exit /b %ERRORLEVEL%')
    & cmd /c $bat *> $log
    $rc = $LASTEXITCODE
    $fails = @(Get-Content (Join-Path $dir 'result.txt') -ErrorAction SilentlyContinue | Where-Object { $_ -match '^FAIL!' } | ForEach-Object { ($_ -split '\s+')[2] })
    $totals = @(Get-Content (Join-Path $dir 'result.txt') -ErrorAction SilentlyContinue | Where-Object { $_ -match '^Totals' }) -join ''
    if ($rc -ge 90) { Say "$name : build error $rc (see $log)"; $failed++; continue }
    if ($name -eq 'original') {
        $ok = $rc -eq 0
        Say "$name : test exit $rc - $totals -> $(if ($ok) { 'passes' } else { 'FAILS (unexpected)' })"
    } else {
        $ok = $rc -ne 0
        Say "$name : test exit $rc - $totals; failed: $($fails -join ' ') -> $(if ($ok) { 'caught' } else { 'NOT caught' })"
    }
    if (-not $ok) { $failed++ }
}
Say "=== $(if ($failed) { "$failed problem(s)" } else { 'original passes, every mutant caught' })"
exit $(if ($failed) { 1 } else { 0 })

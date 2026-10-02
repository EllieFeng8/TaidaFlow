# w2-086 D4: do the tests catch a broken implementation? Each mutant changes one line of
# Core/SensorOffsetStorage.cpp or Core/manager.cpp (exact text replace), rebuilds and runs only
# tst_offset_storage in build\w2-086-run\core-tests, expects it to FAIL, and restores the file (byte
# copy kept in build\w2-086-run\mutant-backup, restored in finally and touched so ninja rebuilds it). A
# baseline run first and a final run at the end use the unchanged sources and must PASS.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-086\tools\mutants.ps1 [-Out docs\evidence\w2-086\d4-mutants.txt]
# Needs a configured build\w2-086-run\core-tests (tools\run-tests-w2086.bat core). Exit 0 = every mutant killed and the
# final run passes.
param([string]$Out = 'docs\evidence\w2-086\d4-mutants.txt')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
$build = Join-Path $root 'build\w2-086-run\core-tests'
$backup = Join-Path $root 'build\w2-086-run\mutant-backup'
New-Item -ItemType Directory -Force $backup | Out-Null
Set-Content -Encoding utf8 $Out "=== w2-086 mutants $(Get-Date -Format o)"
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Write-Output $line; Add-Content -Encoding utf8 $Out $line }

$mutants = @(
    @{ name = 'server PV raw (D1b off)'; file = 'Core\manager.cpp'
       from = 'emit serverInputRegisterUpdated(serverOffset, serverValue);'; to = 'emit serverInputRegisterUpdated(serverOffset, values.at(index));' },
    @{ name = 'sample not corrected (D1 off)'; file = 'Core\SensorOffsetStorage.cpp'
       from = 'readings[column.serverRegister] = double(stored);'; to = 'readings[column.serverRegister] = double(raw);' },
    @{ name = 'floor instead of round'; file = 'Core\SensorOffsetStorage.cpp'
       from = 'std::round(double(raw) + result.offsetCounts)'; to = 'std::floor(double(raw) + result.offsetCounts)' },
    @{ name = 'offset x scale instead of / scale'; file = 'Core\SensorOffsetStorage.cpp'
       from = 'result.offsetCounts = offset / scale;'; to = 'result.offsetCounts = offset * scale;' },
    @{ name = 'no upper clamp'; file = 'Core\SensorOffsetStorage.cpp'
       from = '} else if (corrected > maximum) {'; to = '} else if (corrected > maximum * 4) {' },
    @{ name = 'warning not rate limited'; file = 'Core\SensorOffsetStorage.cpp'
       from = 'if (it->allow(&heldBack)) {'; to = 'if (it->allow(&heldBack) || true) {' },
    @{ name = 'offsets of the start only (settings change ignored)'; file = 'Core\SensorOffsetStorage.cpp'
       from = 'const QVariantMap settings = m_proxy->sensorSettingsSv();   // the offsets of this moment'; to = 'static const QVariantMap settings = m_proxy->sensorSettingsSv();' }
)

function Run-Test([string]$tag) {
    $log = Join-Path $backup "$tag.log"
    & cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && set PATH=C:\Qt\6.8.3\msvc2022_64\bin;C:\Qt\Tools\Ninja;%PATH% && C:\Qt\Tools\CMake_64\bin\cmake.exe --build ""$build"" --target tst_offset_storage && C:\Qt\Tools\CMake_64\bin\ctest.exe --test-dir ""$build"" -R tst_offset_storage --output-on-failure" *> $log
    $rc = $LASTEXITCODE
    $fails = @(Get-Content $log | Where-Object { $_ -match '^FAIL!' } | ForEach-Object { ($_ -replace '^FAIL!\s+:\s+', '') -replace '\s+\S*returned FALSE.*$', '' } | Select-Object -First 3)
    return @{ rc = $rc; fails = $fails; log = $log }
}

$failed = 0
# Baseline: the unchanged sources (touched, so every object file is rebuilt from them) must pass first.
foreach ($f in @('Core\manager.cpp', 'Core\SensorOffsetStorage.cpp')) { (Get-Item (Join-Path $root $f)).LastWriteTime = Get-Date }
$r = Run-Test 'baseline'
Say "baseline run with the unchanged sources: ctest exit=$($r.rc)"
if ($r.rc -ne 0) { Say '=== baseline fails - mutants not run'; exit 1 }
foreach ($m in $mutants) {
    $path = Join-Path $root $m.file
    $copy = Join-Path $backup ((Split-Path $path -Leaf) + '.orig')
    Copy-Item $path $copy -Force
    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        $count = ([regex]::Matches($text, [regex]::Escape($m.from))).Count
        if ($count -ne 1) { Say "MUTANT '$($m.name)': pattern found $count times - not applied"; $failed++; continue }
        $mutated = $text.Replace($m.from, $m.to)
        [System.IO.File]::WriteAllBytes($path, [System.Text.Encoding]::UTF8.GetBytes($mutated))
        (Get-Item $path).LastWriteTime = Get-Date
        $r = Run-Test ('mutant-' + ($m.name -replace '[^A-Za-z0-9]+', '-'))
        $killed = $r.rc -ne 0
        Say ("MUTANT '{0}' ({1}): ctest exit={2} -> {3}; failing: {4}" -f $m.name, $m.file, $r.rc, $(if ($killed) { 'KILLED' } else { 'SURVIVED' }), ($r.fails -join ' | '))
        if (-not $killed) { $failed++ }
    } finally {
        Copy-Item $copy $path -Force
        # Copy-Item keeps the backup's (old) time stamp: touch the restored file so ninja rebuilds it and
        # the next mutant / the final run never uses an object file of this mutant.
        (Get-Item $path).LastWriteTime = Get-Date
    }
}
$r = Run-Test 'final'
Say "final run with the unchanged sources: ctest exit=$($r.rc)"
if ($r.rc -ne 0) { $failed++ }
Say "=== $(if ($failed) { "$failed problem(s)" } else { 'every mutant killed, final run passes' })"
exit $(if ($failed) { 1 } else { 0 })

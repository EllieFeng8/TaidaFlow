# w2-084 D3: negative controls of App/wasm/tests/shell-autoreload.test.js. Each mutant is a copy of
# App/wasm/TaidaFlowApp.shell.html with ONE deliberate defect (written to build\w2-084-mutants, the
# real template is not touched); the node test must FAIL (exit 1) on every mutant and pass on the
# unchanged template.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-084\tools\shell-negative-controls.ps1
# Exit 0 = the real template passes and every mutant is caught.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$node = 'C:\tools\emsdk\node\16.20.0_64bit\bin\node.exe'
$test = Join-Path $root 'App\wasm\tests\shell-autoreload.test.js'
$template = Join-Path $root 'App\wasm\TaidaFlowApp.shell.html'
$outDir = Join-Path $root 'build\w2-084-mutants'
New-Item -ItemType Directory -Force $outDir | Out-Null
$source = [System.IO.File]::ReadAllText($template, [System.Text.Encoding]::UTF8)
$mutants = [ordered]@{
    'M1-countdown-5s'        = @('var CRASH_RELOAD_AFTER_MS = 10000;', 'var CRASH_RELOAD_AFTER_MS = 5000;')
    'M2-no-reload-call'      = @('reload: () => window.location.reload(),', 'reload: () => {},')
    'M3-streak-not-counted'  = @('String(state.streak + 1)', 'String(state.streak)')
    'M4-backoff-max-10min'   = @('var RELOAD_BACKOFF_MAX_MS = 300000;', 'var RELOAD_BACKOFF_MAX_MS = 600000;')
    'M5-abort-not-fatal'     = @('return /Aborted\(/.test(text) || ', 'return ')
    'M6-showError-no-reload' = @("                    scheduleAutoReload();`n", '')
    'M7-syntax-error'        = @('var TaidaFlowAutoReload = (function () {', 'var TaidaFlowAutoReload = (function () {{')
    'M8-other-storage-key'   = @("var STORAGE_STREAK_KEY = 'taidaflow.autoReload.streak';", "var STORAGE_STREAK_KEY = 'taidaflow.reload.streak';")
}
$failed = 0
& $node $test *> (Join-Path $outDir 'original.txt')
$rc = $LASTEXITCODE
"original template: exit $rc (expected 0)"
if ($rc -ne 0) { $failed++ }
foreach ($name in $mutants.Keys) {
    $from, $to = $mutants[$name]
    $from = $from -replace "`r?`n", "`n"
    if (-not $source.Contains($from)) { "${name}: pattern not found - FAIL"; $failed++; continue }
    $file = Join-Path $outDir "$name.shell.html"
    [System.IO.File]::WriteAllText($file, $source.Replace($from, $to), (New-Object System.Text.UTF8Encoding($false)))
    & $node $test --template $file *> (Join-Path $outDir "$name.txt")
    $rc = $LASTEXITCODE
    $totals = (Select-String -Path (Join-Path $outDir "$name.txt") -Pattern '^Totals:' | Select-Object -Last 1).Line
    if (-not $totals) { $totals = (Get-Content (Join-Path $outDir "$name.txt") | Select-Object -First 3) -join ' / ' }
    "${name}: exit $rc ($totals) -> $(if ($rc -ne 0) { 'caught' } else { 'NOT caught' })"
    if ($rc -eq 0) { $failed++ }
}
"result: $(if ($failed) { "$failed problem(s)" } else { 'original passes, every mutant caught' })"
exit $(if ($failed) { 1 } else { 0 })

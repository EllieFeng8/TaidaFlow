# w2-071 / w2-072 D7: every CTest / QTest project of the work tree, one after the other (all build into
# taidaflow\build\...). Each suite's console output goes to docs\evidence\w2-071\30-ctest-<n>-<name>.log and
# its exit code to 30-ctest-summary.txt. The bench data of the w2-041/045/052 harnesses is made by the C++
# generator (docs\evidence\w2-062\tools\make-bench-db.bat, no Python). The export harness needs port 8124 free.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-071\tools\run-all-ctest.ps1
# Exit 0 = every suite exit code 0.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$ev = Join-Path $root 'docs\evidence\w2-071'
$summary = Join-Path $ev '30-ctest-summary.txt'
Set-Content -Path $summary -Value ("=== all CTest suites {0}" -f (Get-Date -Format o)) -Encoding utf8
$suites = @(
    @{ n = '01'; name = 'core-tests (Core\tests: w2-071 + w2-072, fresh)'; cmd = 'Core\tests\run-core-tests.bat fresh' },
    @{ n = '02'; name = 'app-tests (App\tests)'; cmd = 'docs\evidence\w2-065\tools\run-app-tests.bat' },
    @{ n = '03'; name = 'apphttpserver (Core\AppHttpServer\tests)'; cmd = 'scripts\run-apphttpserver-tests.bat' },
    @{ n = '04'; name = 'bench data (C++ generator, not a test)'; cmd = 'docs\evidence\w2-062\tools\make-bench-db.bat' },
    @{ n = '05'; name = 'w2-041/w2-049 export + range harness (HistoryExport, SqlManager)'; cmd = 'docs\evidence\w2-049\tools\run-w2041-qtest.bat' },
    @{ n = '06'; name = 'w2-045 stepped History range (SqlManager)'; cmd = 'docs\evidence\w2-045\tools\run-qtest.bat' },
    @{ n = '07'; name = 'w2-052 History views (SqlManager, HistoryExport)'; cmd = 'docs\evidence\w2-052\tools\run-qtest.bat' },
    @{ n = '08'; name = 'w2-053 DI restart (SqlManager)'; cmd = 'docs\evidence\w2-053\tools\run-qtest.bat' },
    @{ n = '09'; name = 'w2-067 SqlManager shutdown'; cmd = 'docs\evidence\w2-067\tools\run-qtest.bat' },
    @{ n = '10'; name = 'wasm-mirror pack native tests'; cmd = 'scripts\run-pack-tests.bat' }
)
$failed = 0
Push-Location $root
try {
    foreach ($s in $suites) {
        $log = Join-Path $ev ("30-ctest-{0}.log" -f $s.n)
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & cmd.exe /c ($s.cmd + ' > "' + $log + '" 2>&1')
        $rc = $LASTEXITCODE
        $tail = @(Get-Content -LiteralPath $log | Where-Object { $_ -match 'tests passed|Totals:|tests failed' } | Select-Object -Last 6) -join ' | '
        $line = "{0} {1}: exit code {2} ({3} s) {4}" -f $s.n, $s.name, $rc, [int]$sw.Elapsed.TotalSeconds, $tail
        Add-Content -Path $summary -Value $line -Encoding utf8
        [Console]::Out.WriteLine($line)
        if ($rc -ne 0) { $failed++ }
    }
} finally { Pop-Location }
Add-Content -Path $summary -Value ("=== {0} suite(s) with a non-zero exit code" -f $failed) -Encoding utf8
exit $failed

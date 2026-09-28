# w2-065: the build checks of docs\BUILD.md section 7, each with its exit code, output into
# docs\evidence\w2-065\40-checks\<name>.txt. Needs build\desktop and build\wasm-release (built first).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-065\tools\run-checks.ps1
# Exit 0 = every check exit 0.
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$out = Join-Path $root 'docs\evidence\w2-065\40-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$checks = [ordered]@{
    'check-wasm-backend'   = @('scripts\check-wasm-backend.ps1', 'build\desktop', 'build\wasm-release')
    'check-version-shadow' = @('scripts\check-version-shadow.ps1', 'build\desktop', 'build\wasm-release')
    'check-rest-routes'    = @('scripts\check-rest-routes.ps1')
    'verify-pack'          = @('scripts\verify-pack.ps1', '-Source', '..\WebAssemblyTest\integration-pack\wasm-mirror')
    'make-font-subset'     = @('scripts\make-font-subset.ps1', '--check')
}
$fails = 0
$summary = New-Object System.Collections.Generic.List[string]
Push-Location $root
try {
    foreach ($name in $checks.Keys) {
        $a = $checks[$name]
        $text = & $ps -NoProfile -ExecutionPolicy Bypass -File $a[0] @($a | Select-Object -Skip 1) 2>&1 | ForEach-Object { "$_" }
        $rc = $LASTEXITCODE
        [System.IO.File]::WriteAllLines((Join-Path $out "$name.txt"), [string[]](@($text) + "exit code $rc"), (New-Object System.Text.UTF8Encoding($false)))
        if ($rc -ne 0) { $fails++ }
        $line = "{0,-22} exit {1}   last line: {2}" -f $name, $rc, (@($text) | Where-Object { "$_".Trim() -ne '' } | Select-Object -Last 1)
        $summary.Add($line); [Console]::Out.WriteLine($line)
    }
} finally { Pop-Location }
$summary.Add("=== $fails check(s) failed")
[System.IO.File]::WriteAllLines((Join-Path $out 'summary.txt'), $summary.ToArray(), (New-Object System.Text.UTF8Encoding($false)))
[Console]::Out.WriteLine("=== $fails check(s) failed")
if ($fails) { exit 1 }
exit 0

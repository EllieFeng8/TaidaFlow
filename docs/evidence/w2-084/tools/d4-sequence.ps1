# w2-084 D0/D3/D4: build and check sequence, in this order, each step's output in <OutDir>\d4-<step>.log
# and one line per step in <OutDir>\d4-summary.txt:
#   font --check -> desktop fresh -> wasm-release fresh -> shell page tests (node, with the built page)
#   -> check-wasm-backend -> check-version-shadow -> verify-pack -> check-rest-routes
#   -> contract hash desktop == wasm -> Core tests fresh -> App tests fresh
# Before every build step: waits until no cl / clang / clang++ / wasm-ld / ninja process runs (process
# names; up to 30 min) and logs what it saw.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-084\tools\d4-sequence.ps1 [-OutDir docs\evidence\w2-084]
# Exit 0 = every step exit 0 (and the contract hashes are equal).
param([string]$OutDir = 'docs\evidence\w2-084')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'd4-summary.txt'
Set-Content -Encoding utf8 $summary "=== w2-084 d4 sequence $(Get-Date -Format o)"
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Write-Output $line; Add-Content -Encoding utf8 $summary $line }
function Wait-Idle([string]$step) {
    $deadline = (Get-Date).AddMinutes(30)
    while ($true) {
        $busy = @(Get-Process cl, clang, clang++, wasm-ld, ninja -ErrorAction SilentlyContinue)
        if ($busy.Count -eq 0) { Say "[$step] idle check: no cl/clang/clang++/wasm-ld/ninja"; return }
        Say "[$step] busy: $(($busy | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ') - waiting"
        if ((Get-Date) -gt $deadline) { Say "[$step] still busy after 30 min"; exit 1 }
        Start-Sleep -Seconds 20
    }
}
$failed = 0
function Step([string]$name, [scriptblock]$body, [switch]$Build) {
    if ($Build) { Wait-Idle $name }
    $log = Join-Path $OutDir "d4-$name.log"
    $start = Get-Date
    & $body *> $log
    $rc = $LASTEXITCODE
    Add-Content $log "exit=$rc"
    # PowerShell 5.1 writes *> as UTF-16: store the log as UTF-8 (no BOM) so findstr / Select-String read it.
    $text = [System.IO.File]::ReadAllText($log)
    [System.IO.File]::WriteAllText($log, $text, (New-Object System.Text.UTF8Encoding($false)))
    Say ("{0}: exit={1} ({2:n0} s)" -f $name, $rc, ((Get-Date) - $start).TotalSeconds)
    if ($rc -ne 0) { $script:failed++ }
    return $rc
}
$ps = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File')

$null = Step 'font-check' { & powershell @ps scripts\make-font-subset.ps1 --check }
$null = Step 'build-desktop-fresh' { & cmd /c "scripts\build-desktop.bat fresh" } -Build
$null = Step 'build-wasm-release-fresh' { & cmd /c "scripts\build-wasm.bat wasm-release fresh" } -Build
$null = Step 'shell-tests' { & cmd /c "App\wasm\tests\run-shell-tests.bat build\wasm-release\TaidaFlowApp.html" }
$null = Step 'check-wasm-backend' { & powershell @ps scripts\check-wasm-backend.ps1 build\desktop build\wasm-release }
$null = Step 'check-version-shadow' { & powershell @ps scripts\check-version-shadow.ps1 build\desktop build\wasm-release }
$null = Step 'verify-pack' { & powershell @ps scripts\verify-pack.ps1 }
$null = Step 'check-rest-routes' { & powershell @ps scripts\check-rest-routes.ps1 }
$null = Step 'contract-hash' { & powershell @ps docs\evidence\w2-084\tools\contract-hash.ps1 }
$null = Step 'core-tests-fresh' { & cmd /c "Core\tests\run-core-tests.bat fresh" } -Build
$null = Step 'app-tests-fresh' { & cmd /c "App\tests\run-app-tests.bat fresh" } -Build

Say "=== $(if ($failed) { "$failed step(s) FAILED" } else { 'all steps exit 0' })"
exit $(if ($failed) { 1 } else { 0 })

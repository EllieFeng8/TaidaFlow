# w2-087 D4: post-merge build and check sequence in the task's own folders (adapted from
# docs\evidence\w2-086\tools\d5-sequence.ps1). Each step's output in <OutDir>\d4-<step>.log and one line
# per step in <OutDir>\d4-summary.txt:
#   font --check -> desktop fresh (build\w2-087-desktop) -> wasm-release fresh (build\w2-087-wasm)
#   -> check-wasm-backend -> check-version-shadow -> verify-pack -> check-rest-routes
#   -> DeviceStatusPublisher only in the desktop build -> Core tests fresh -> App tests fresh
#   (tests in build\w2-087-run\core-tests and build\w2-087-run\app-tests).
# build\desktop, build\wasm-release, build\core-tests, build\app-tests (and taidaflow-main / taidaflow-core)
# are never touched. font-check = scripts\make-font-subset.ps1 --check (the project's one allowed font
# tool, run through its ps1).
# Before every build step: waits until no cl / clang / clang++ / wasm-ld / ninja process runs (other
# builds on this machine finish first); the process list of each check is in the summary.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-087\tools\d4-sequence.ps1 [-OutDir docs\evidence\w2-087]
# Exit 0 = every step exit 0.
param([string]$OutDir = 'docs\evidence\w2-087')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'd4-summary.txt'
Set-Content -Encoding utf8 $summary "=== w2-087 d4 sequence $(Get-Date -Format o) HEAD $(git rev-parse --short HEAD)"
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Write-Output $line; Add-Content -Encoding utf8 $summary $line }
function Wait-Idle([string]$step) {
    $deadline = (Get-Date).AddMinutes(60)
    while ($true) {
        $busy = @(Get-Process cl, clang, clang++, wasm-ld, ninja -ErrorAction SilentlyContinue)
        if ($busy.Count -eq 0) { Say "[$step] idle check: no cl/clang/clang++/wasm-ld/ninja"; return }
        Say "[$step] busy: $(($busy | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ') - waiting"
        if ((Get-Date) -gt $deadline) { Say "[$step] still busy after 60 min"; exit 1 }
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
    $text = [System.IO.File]::ReadAllText($log)
    [System.IO.File]::WriteAllText($log, $text, (New-Object System.Text.UTF8Encoding($false)))
    Say ("{0}: exit={1} ({2:n0} s)" -f $name, $rc, ((Get-Date) - $start).TotalSeconds)
    if ($rc -ne 0) { $script:failed++ }
}
$ps = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File')
$tools = 'docs\evidence\w2-087\tools'

Step 'font-check' { & powershell @ps scripts\make-font-subset.ps1 --check }
Step 'build-desktop-fresh' { & cmd /c "$tools\build-w2087.bat desktop fresh" } -Build
Step 'build-wasm-release-fresh' { & cmd /c "$tools\build-w2087.bat wasm fresh" } -Build
Step 'check-wasm-backend' { & powershell @ps scripts\check-wasm-backend.ps1 build\w2-087-desktop build\w2-087-wasm }
Step 'check-version-shadow' { & powershell @ps scripts\check-version-shadow.ps1 build\w2-087-desktop build\w2-087-wasm }
Step 'verify-pack' { & powershell @ps scripts\verify-pack.ps1 }
Step 'check-rest-routes' { & powershell @ps scripts\check-rest-routes.ps1 }
Step 'devicestatus-desktop-only' {
    # The new source is compiled into the desktop app only (ninja's command list of each target); the
    # deviceStatus property itself (w1-087, TaidaFlowProxy.h) is in both (mirrored).
    $d = @(& 'C:\Qt\Tools\Ninja\ninja.exe' -C build\w2-087-desktop -t commands TaidaFlowApp | Select-String 'DeviceStatusPublisher\.cpp')
    $w = @(& 'C:\Qt\Tools\Ninja\ninja.exe' -C build\w2-087-wasm -t commands TaidaFlowApp | Select-String 'DeviceStatusPublisher\.cpp')
    $exe = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\w2-087-desktop\TaidaFlowApp.exe'))
    $wasm = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\w2-087-wasm\TaidaFlowApp.wasm'))
    # QStringLiteral text is UTF-16 in the MSVC binary and UTF-8/ASCII in the wasm: look for both.
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    function Has([string]$bin, [string]$text) {
        $n8 = $latin1.GetString([System.Text.Encoding]::UTF8.GetBytes($text))
        $n16 = $latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($text))
        return ($bin.Contains($n8) -or $bin.Contains($n16))
    }
    $exeText = $latin1.GetString($exe); $wasmText = $latin1.GetString($wasm)
    $logText = '[DeviceStatus] stopped: deviceStatus = {} (unknown)'
    $inExe = Has $exeText $logText; $inWasm = Has $wasmText $logText
    $propExe = Has $exeText 'deviceStatusChanged'; $propWasm = Has $wasmText 'deviceStatusChanged'
    Write-Output "desktop: DeviceStatusPublisher.cpp compile commands=$($d.Count), log string in TaidaFlowApp.exe=$inExe, deviceStatusChanged=$propExe"
    Write-Output "wasm   : DeviceStatusPublisher.cpp compile commands=$($w.Count), log string in TaidaFlowApp.wasm=$inWasm, deviceStatusChanged=$propWasm"
    $global:LASTEXITCODE = $(if ($d.Count -ge 1 -and $inExe -and $propExe -and $w.Count -eq 0 -and -not $inWasm -and $propWasm) { 0 } else { 1 })
}
Step 'core-tests-fresh' { & cmd /c "$tools\run-tests-w2087.bat core fresh" } -Build
Step 'app-tests-fresh' { & cmd /c "$tools\run-tests-w2087.bat app fresh" } -Build

Say "=== $(if ($failed) { "$failed step(s) FAILED" } else { 'all steps exit 0' })"
exit $(if ($failed) { 1 } else { 0 })

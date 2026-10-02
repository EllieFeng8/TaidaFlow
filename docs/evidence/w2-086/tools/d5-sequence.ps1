# w2-086 D4/D5: build and check sequence in the task's own folders, each step's output in
# <OutDir>\d5-<step>.log and one line per step in <OutDir>\d5-summary.txt:
#   font --check -> desktop fresh (build\w2-086-desktop) -> wasm-release fresh (build\w2-086-wasm)
#   -> check-wasm-backend -> check-version-shadow -> verify-pack -> check-rest-routes
#   -> SensorOffsetStorage only in the desktop build -> Core tests fresh -> App tests fresh
#   (tests in build\w2-086-run\core-tests and build\w2-086-run\app-tests).
# build\desktop, build\wasm-release, build\core-tests, build\app-tests (and the taidaflow-core clone)
# are never touched. font-check = scripts\make-font-subset.ps1 --check (the project's one allowed Python tool,
# run through its ps1; the new Core comments brought 2 new CJK chars, App\fonts was regenerated with it).
# Before every build step: waits until no cl / clang / clang++ / wasm-ld / ninja process runs (other
# builds on this machine, e.g. the PM's in taidaflow-core, finish first).
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-086\tools\d5-sequence.ps1 [-OutDir docs\evidence\w2-086]
# Exit 0 = every step exit 0.
param([string]$OutDir = 'docs\evidence\w2-086')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'd5-summary.txt'
Set-Content -Encoding utf8 $summary "=== w2-086 d5 sequence $(Get-Date -Format o)"
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
    $log = Join-Path $OutDir "d5-$name.log"
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
$tools = 'docs\evidence\w2-086\tools'

Step 'font-check' { & powershell @ps scripts\make-font-subset.ps1 --check }
Step 'build-desktop-fresh' { & cmd /c "$tools\build-w2086.bat desktop fresh" } -Build
Step 'build-wasm-release-fresh' { & cmd /c "$tools\build-w2086.bat wasm fresh" } -Build
Step 'check-wasm-backend' { & powershell @ps scripts\check-wasm-backend.ps1 build\w2-086-desktop build\w2-086-wasm }
Step 'check-version-shadow' { & powershell @ps scripts\check-version-shadow.ps1 build\w2-086-desktop build\w2-086-wasm }
Step 'verify-pack' { & powershell @ps scripts\verify-pack.ps1 }
Step 'check-rest-routes' { & powershell @ps scripts\check-rest-routes.ps1 }
Step 'sensoroffset-desktop-only' {
    # The new source is compiled into the desktop app only (ninja's command list of each target).
    $d = @(& 'C:\Qt\Tools\Ninja\ninja.exe' -C build\w2-086-desktop -t commands TaidaFlowApp | Select-String 'SensorOffsetStorage\.cpp')
    $w = @(& 'C:\Qt\Tools\Ninja\ninja.exe' -C build\w2-086-wasm -t commands TaidaFlowApp | Select-String 'SensorOffsetStorage\.cpp')
    $exe = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\w2-086-desktop\TaidaFlowApp.exe'))
    $wasm = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\w2-086-wasm\TaidaFlowApp.wasm'))
    # QStringLiteral text is UTF-16 in the MSVC binary and UTF-8/ASCII in the wasm: look for both.
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $text = '[SensorOffset] offsets are applied to sensor_data'
    $n8 = $latin1.GetString([System.Text.Encoding]::UTF8.GetBytes($text))
    $n16 = $latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($text))
    $exeText = $latin1.GetString($exe); $wasmText = $latin1.GetString($wasm)
    $inExe = $exeText.Contains($n8) -or $exeText.Contains($n16)
    $inWasm = $wasmText.Contains($n8) -or $wasmText.Contains($n16)
    Write-Output "desktop: SensorOffsetStorage.cpp compile commands=$($d.Count), log string in TaidaFlowApp.exe=$inExe"
    Write-Output "wasm   : SensorOffsetStorage.cpp compile commands=$($w.Count), log string in TaidaFlowApp.wasm=$inWasm"
    $global:LASTEXITCODE = $(if ($d.Count -ge 1 -and $inExe -and $w.Count -eq 0 -and -not $inWasm) { 0 } else { 1 })
}
Step 'core-tests-fresh' { & cmd /c "$tools\run-tests-w2086.bat core fresh" } -Build
Step 'app-tests-fresh' { & cmd /c "$tools\run-tests-w2086.bat app fresh" } -Build

Say "=== $(if ($failed) { "$failed step(s) FAILED" } else { 'all steps exit 0' })"
exit $(if ($failed) { 1 } else { 0 })

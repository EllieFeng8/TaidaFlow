# w2-089 D4: post-merge build and check sequence in the task's own folders (adapted from
# docs\evidence\w2-086\tools\d5-sequence.ps1). Each step's output in <OutDir>\d4-<step>.log and one line
# per step in <OutDir>\d4-summary.txt:
#   font --check -> desktop fresh (build\w2-089-desktop) -> wasm-release fresh (build\w2-089-wasm)
#   -> check-wasm-backend -> check-version-shadow -> verify-pack -> check-rest-routes
#   -> DeviceStatusPublisher only in the desktop build -> Core tests fresh -> App tests fresh
#   (tests in build\w2-089-run\core-tests and build\w2-089-run\app-tests).
# build\desktop, build\wasm-release, build\core-tests, build\app-tests (and taidaflow-main / taidaflow-core)
# are never touched. font-check = scripts\make-font-subset.ps1 --check (the project's one allowed font
# tool, run through its ps1).
# Before every build step: waits until no cl / clang / clang++ / wasm-ld / ninja process runs (other
# builds on this machine finish first); the process list of each check is in the summary.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-089\tools\d4-sequence.ps1 [-OutDir docs\evidence\w2-089]
# Exit 0 = every step exit 0.
param([string]$OutDir = 'docs\evidence\w2-089')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$summary = Join-Path $OutDir 'd4-summary.txt'
Set-Content -Encoding utf8 $summary "=== w2-089 d4 sequence $(Get-Date -Format o) HEAD $(git rev-parse --short HEAD)"
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
$tools = 'docs\evidence\w2-089\tools'

Step 'font-check' { & powershell @ps scripts\make-font-subset.ps1 --check }
Step 'build-desktop-fresh' { & cmd /c "$tools\build-w2089.bat desktop fresh" } -Build
Step 'build-wasm-release-fresh' { & cmd /c "$tools\build-w2089.bat wasm fresh" } -Build
Step 'check-wasm-backend' { & powershell @ps scripts\check-wasm-backend.ps1 build\w2-089-desktop build\w2-089-wasm }
Step 'check-version-shadow' { & powershell @ps scripts\check-version-shadow.ps1 build\w2-089-desktop build\w2-089-wasm }
Step 'verify-pack' { & powershell @ps scripts\verify-pack.ps1 }
Step 'check-rest-routes' { & powershell @ps scripts\check-rest-routes.ps1 }
Step 'devicestatus-desktop-only' {
    # The new source is compiled into the desktop app only (ninja's command list of each target); the
    # deviceStatus property itself (w1-087, TaidaFlowProxy.h) is in both (mirrored).
    $d = @(& 'C:\Qt\Tools\Ninja\ninja.exe' -C build\w2-089-desktop -t commands TaidaFlowApp | Select-String 'DeviceStatusPublisher\.cpp')
    $w = @(& 'C:\Qt\Tools\Ninja\ninja.exe' -C build\w2-089-wasm -t commands TaidaFlowApp | Select-String 'DeviceStatusPublisher\.cpp')
    $exe = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\w2-089-desktop\TaidaFlowApp.exe'))
    $wasm = [System.IO.File]::ReadAllBytes((Join-Path $root 'build\w2-089-wasm\TaidaFlowApp.wasm'))
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
    # w2-089: the new name (U+5FAA U+74B0 U+6CF5 U+8B8A U+983B U+5668, written as code points so this
    # ps1 needs no BOM) is in the desktop app (UTF-16 QStringLiteral of DeviceStatusPublisher / manager.cpp).
    $newName = -join ([char[]](0x5FAA, 0x74B0, 0x6CF5, 0x8B8A, 0x983B, 0x5668))
    $nameExe = Has $exeText $newName
    Write-Output "desktop: MS300 name (circulation pump inverter, 6 CJK chars) in TaidaFlowApp.exe=$nameExe"
    $global:LASTEXITCODE = $(if ($d.Count -ge 1 -and $inExe -and $propExe -and $w.Count -eq 0 -and -not $inWasm -and $propWasm -and $nameExe) { 0 } else { 1 })
}
# w2-089: every character of the new name is in the embedded font subset (charset.txt and both TTF
# files are the committed ones; make-font-subset --check above verifies the subset covers every
# source text, this step shows the six characters explicitly).
Step 'font-has-new-name' {
    $chars = [System.IO.File]::ReadAllText((Join-Path $root 'App\fonts\charset.txt'), [System.Text.Encoding]::UTF8)
    $missing = @((0x5FAA, 0x74B0, 0x6CF5, 0x8B8A, 0x983B, 0x5668) | Where-Object { -not $chars.Contains([string][char]$_) })
    Write-Output ("App\fonts\charset.txt: {0} chars; missing of U+5FAA U+74B0 U+6CF5 U+8B8A U+983B U+5668: {1}" -f $chars.Length, $(if ($missing.Count) { ($missing | ForEach-Object { 'U+{0:X4}' -f $_ }) -join ' ' } else { 'none' }))
    $global:LASTEXITCODE = $(if ($missing.Count -eq 0) { 0 } else { 1 })
}
Step 'core-tests-fresh' { & cmd /c "$tools\run-tests-w2089.bat core fresh" } -Build
Step 'app-tests-fresh' { & cmd /c "$tools\run-tests-w2089.bat app fresh" } -Build

Say "=== $(if ($failed) { "$failed step(s) FAILED" } else { 'all steps exit 0' })"
exit $(if ($failed) { 1 } else { 0 })

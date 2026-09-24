# w2-043 D2 self-test: scripts\safety_probe.ps1 must treat a listener on 18125 (the desktop's
# internal mirror port) as a stale instance -> verdict BUSY, exit 4.
# A test-owned Python listener (ws_probe.py occupy) holds 127.0.0.1:18125 - the same address the
# desktop Mirror server binds. No app is started; nothing that this script did not start is stopped.
# Steps:
#   0. precondition: no listener on 502/8124/8125/18125 (otherwise exit 2, nothing touched)
#   1. baseline probe                       -> expect exit 0 (SAFE)
#   2. fake listener on 127.0.0.1:18125     -> expect exit 4, verdict BUSY, "port 18125 (internal mirror)"
#   3. fake listener stopped, probe again   -> expect exit 0 (SAFE)
# Probe records go to docs\evidence\w2-043\safety-probe-selftest.log; summary to
# docs\evidence\w2-043\probe-selftest-18125.txt.
# Usage: powershell -ExecutionPolicy Bypass -File docs\evidence\w2-043\tools\probe_selftest_18125.ps1
# Exit 0 = all three expectations met; 1 = a check failed; 2 = precondition failed.
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$ev    = Split-Path -Parent $tools
$root  = (Resolve-Path (Join-Path $ev '..\..\..')).Path
$probeScript = Join-Path $root 'scripts\safety_probe.ps1'
$probeLog = Join-Path $ev 'safety-probe-selftest.log'
$summary  = Join-Path $ev 'probe-selftest-18125.txt'
$py = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'
if (Test-Path $summary) { Remove-Item $summary -Force }
function Note($m) { Write-Output $m; Add-Content -Path $summary -Value $m -Encoding utf8 }
function RunProbe($reason) {
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $probeScript -Reason $reason -LogFile $probeLog
    $rc = $LASTEXITCODE
    $o | ForEach-Object { Note "    $_" } | Out-Host      # keep the function's return value clean
    Note "  -> exit $rc" | Out-Host
    return @{ rc = $rc; text = ($o -join "`n") }
}
$fail = @()

$pre = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 502, 8124, 8125, 18125 })
if ($pre.Count) { Note "precondition failed: listeners $($pre | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)/pid $($_.OwningProcess)" }) - nothing touched"; exit 2 }

Note "[1] baseline (no 18125 listener)"
$r1 = RunProbe 'w2-043 selftest 1: baseline'
if ($r1.rc -ne 0) { $fail += "baseline exit $($r1.rc) (expected 0)" }

Note "[2] fake listener on 127.0.0.1:18125"
$occ = Start-Process $py -ArgumentList "-B `"$(Join-Path $tools 'ws_probe.py')`" occupy 18125 60 127.0.0.1" -PassThru -WindowStyle Hidden
for ($i = 0; $i -lt 50 -and -not (Get-NetTCPConnection -State Listen -LocalPort 18125 -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
$l = Get-NetTCPConnection -State Listen -LocalPort 18125 -ErrorAction SilentlyContinue | Select-Object -First 1
Note "  occupier pid $($occ.Id); listener $($l.LocalAddress):$($l.LocalPort) pid $($l.OwningProcess)"
try {
    $r2 = RunProbe 'w2-043 selftest 2: fake 18125 listener'
    if ($r2.rc -ne 4) { $fail += "fake 18125 listener: exit $($r2.rc) (expected 4)" }
    if ($r2.text -notmatch 'port 18125 \(internal mirror\) already in use') { $fail += 'no 18125 BUSY line' }
    if ($r2.text -notmatch 'verdict: BUSY') { $fail += 'verdict not BUSY' }
} finally {
    if (-not $occ.HasExited) { Stop-Process -Id $occ.Id -Force }   # our own test listener only
    $occ.WaitForExit(5000) | Out-Null
}
Start-Sleep -Seconds 1

Note "[3] fake listener stopped"
$r3 = RunProbe 'w2-043 selftest 3: after fake listener stopped'
if ($r3.rc -ne 0) { $fail += "after stop exit $($r3.rc) (expected 0)" }

if ($fail.Count -eq 0) { Note 'RESULT PASS (baseline 0, 18125 listener 4 BUSY, after 0)'; exit 0 }
Note "RESULT FAIL: $($fail -join '; ')"; exit 1

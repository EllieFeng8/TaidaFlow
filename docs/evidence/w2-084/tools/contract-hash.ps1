# w2-084 D4: the Mirror contract of the desktop build and of the WebAssembly build are the same.
# The pack's contract hash (integration-pack/wasm-mirror, proxymirror.cpp buildContract) is a SHA-256
# of the property / request-signal list read from TaidaFlowProxy's meta-object, i.e. from moc's output.
# This script compares moc's output for Core/TaidaFlowProxy.h of both builds (the Core_autogen file and
# the relay copy of wasm_mirror_register_proxy): byte-identical files = identical meta-objects = the
# same contract hash on both sides. It also checks that the new property serverHeartbeatMs is in it.
# The hash value itself is printed by the mirror test client (docs\evidence\w2-084\tools\
# mirror-heartbeat-client, "contractHash=...") and accepted by the running desktop in the live smoke.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-084\tools\contract-hash.ps1
#            [-Desktop build\desktop] [-Wasm build\wasm-release]
param([string]$Desktop = 'build\desktop', [string]$Wasm = 'build\wasm-release')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
$failed = 0
foreach ($pattern in @('moc_TaidaFlowProxy.cpp', 'moc_TaidaFlowProxy_*_for_relay.cpp')) {
    $d = @(Get-ChildItem -Recurse -File (Join-Path $Desktop 'Core') -Filter $pattern)
    $w = @(Get-ChildItem -Recurse -File (Join-Path $Wasm 'Core') -Filter $pattern)
    if ($d.Count -ne 1 -or $w.Count -ne 1) { "${pattern}: expected one file per build, found desktop $($d.Count), wasm $($w.Count) - FAIL"; $failed++; continue }
    $hd = (Get-FileHash -Algorithm SHA256 $d[0].FullName).Hash.ToLowerInvariant()
    $hw = (Get-FileHash -Algorithm SHA256 $w[0].FullName).Hash.ToLowerInvariant()
    $text = [System.IO.File]::ReadAllText($d[0].FullName)
    $hasHeartbeat = $text.Contains('"serverHeartbeatMs"') -and $text.Contains('"serverHeartbeatMsChanged"')
    "${pattern}:"
    "  desktop $($d[0].FullName.Substring($root.Length + 1)) sha256=$hd ($($d[0].Length) bytes, $($d[0].LastWriteTime.ToString('s')))"
    "  wasm    $($w[0].FullName.Substring($root.Length + 1)) sha256=$hw ($($w[0].Length) bytes, $($w[0].LastWriteTime.ToString('s')))"
    "  identical=$($hd -eq $hw) serverHeartbeatMs/serverHeartbeatMsChanged in the meta-object=$hasHeartbeat"
    if ($hd -ne $hw -or -not $hasHeartbeat) { $failed++ }
}
"result: $(if ($failed) { 'FAIL' } else { 'desktop and wasm meta-objects identical (same contract hash), serverHeartbeatMs included' })"
exit $(if ($failed) { 1 } else { 0 })

# w2-087 D3: mutation check of tst_device_status (both CTest entries). Each mutant changes one rule of
# the device-status writer in the source, rebuilds the Core tests (build\w2-087-run\core-tests, configured
# by run-tests-w2087.bat core) and runs ctest -R tst_device_status; the mutant must be KILLED (ctest
# exit != 0). The source file is restored from a byte copy after every mutant (and touched, so the next
# build sees it), and the final run must pass again.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-087\tools\mutants.ps1 [-Out build\w2-087-run\mutants.txt]
# Exit 0 = baseline passed, every mutant KILLED (except M7, documented as an expected survivor on a PC
# without serial port), final passed, sources byte-identical to the start.
param([string]$Out = 'build\w2-087-run\mutants.txt')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
Set-Content -Encoding utf8 $Out "=== w2-087 mutants $(Get-Date -Format o)"
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Write-Output $line; Add-Content -Encoding utf8 $Out $line }
$enc = New-Object System.Text.UTF8Encoding($false)
function RunTests([string]$tag) {
    while (@(Get-Process cl, clang, clang++, wasm-ld, ninja -ErrorAction SilentlyContinue).Count) { Start-Sleep -Seconds 10 }
    $log = Join-Path (Split-Path -Parent $Out) "mutant-$tag.log"
    & cmd /c "docs\evidence\w2-087\tools\run-tests-w2087.bat core -R tst_device_status" *> $log
    return $LASTEXITCODE
}
$mutants = @(
    @{ id = 'M1-rewrite-same-state'; file = 'Core\DeviceStatusPublisher.cpp';
       old = "    if (it->state == state)`n        return;"; new = "    if (false && it->state == state)`n        return;" },
    @{ id = 'M2-report-before-first-result'; file = 'Core\DeviceStatusPublisher.cpp';
       old = "        if (it == m_entries.constEnd() || it->state == State::Unknown)"; new = "        if (it == m_entries.constEnd())" },
    @{ id = 'M3-name-is-displayName'; file = 'Core\DeviceStatusPublisher.cpp';
       old = "        entry.name = modelOf(config.device);"; new = "        entry.name = ModbusClient::displayName(config.device);" },
    @{ id = 'M4-stop-writes-nothing'; file = 'Core\DeviceStatusPublisher.cpp';
       old = "    qInfo().noquote() << QStringLiteral(`"[DeviceStatus] stopped: deviceStatus = {} (unknown)`");`n    write(QVariantMap());";
       new = "    qInfo().noquote() << QStringLiteral(`"[DeviceStatus] stopped: deviceStatus = {} (unknown)`");" },
    @{ id = 'M5-ms300-online-on-port-open'; file = 'Core\DeviceStatusPublisher.cpp';
       old = "    if (!open)`n        setOnline(kMs300Key, false, QStringLiteral(`"serial port not open`"));";
       new = "    setOnline(kMs300Key, open, QStringLiteral(`"serial port`"));" },
    @{ id = 'M6-manager-stops-devices-first'; file = 'Core\manager.cpp';
       old = "    m_deviceStatus->stop();      // w2-087: empty map first - the disconnects below are not reported`r`n    m_pollTimer.stop();`r`n    m_ms300FaultReader.stop();`r`n    m_modbus.disconnectAll();";
       new = "    m_pollTimer.stop();`r`n    m_ms300FaultReader.stop();`r`n    m_modbus.disconnectAll();`r`n    m_deviceStatus->stop();" },
    # M7 only differs when a serial port really opens (Connecting -> Connected: MS300 would flash offline
    # before its first answer). This PC has no serial port (the open always fails, Connecting -> Unconnected
    # gives the same single offline result), so it cannot be killed here: run and reported, expected to survive.
    @{ id = 'M7-offline-on-connecting'; file = 'Core\Ms300FaultReader.cpp'; expectSurvive = $true;
       old = "        if (connected || state == QModbusDevice::UnconnectedState)`n            emit portOpenChanged(connected);";
       new = "        emit portOpenChanged(connected);" }
)
$files = @($mutants | ForEach-Object { $_.file } | Sort-Object -Unique)
$backup = @{}
foreach ($f in $files) { $backup[$f] = [System.IO.File]::ReadAllBytes((Join-Path $root $f)) }
$failed = 0
$rc = RunTests 'baseline'
Say "baseline: ctest exit=$rc"
if ($rc -ne 0) { Say 'baseline failed - stop'; exit 1 }
foreach ($m in $mutants) {
    $path = Join-Path $root $m.file
    $text = [System.IO.File]::ReadAllText($path, $enc)
    if ($text.IndexOf($m.old) -lt 0) { Say "$($m.id): pattern not found in $($m.file) - ERROR"; $failed++; continue }
    [System.IO.File]::WriteAllText($path, $text.Replace($m.old, $m.new), $enc)
    $rc = RunTests $m.id
    [System.IO.File]::WriteAllBytes($path, $backup[$m.file])
    (Get-Item $path).LastWriteTime = Get-Date
    $killed = $rc -ne 0
    $why = @(Get-Content (Join-Path (Split-Path -Parent $Out) "mutant-$($m.id).log") -Encoding utf8 | Where-Object { $_ -match '^FAIL!' } | Select-Object -First 2) -join ' / '
    $note = if ($m.expectSurvive) { ' (expected survivor: needs a serial port that opens - none on this PC)' } else { '' }
    Say "$($m.id) ($($m.file)): ctest exit=$rc -> $(if ($killed) { 'KILLED' } else { 'SURVIVED' })$note $why"
    if (-not $killed -and -not $m.expectSurvive) { $failed++ }
}
$rc = RunTests 'final'
Say "final: ctest exit=$rc"
if ($rc -ne 0) { $failed++ }
foreach ($f in $files) {
    $same = [System.Linq.Enumerable]::SequenceEqual([byte[]]$backup[$f], [byte[]][System.IO.File]::ReadAllBytes((Join-Path $root $f)))
    Say "$f restored byte-identical: $same"
    if (-not $same) { $failed++ }
}
Say "=== $(if ($failed) { "$failed problem(s)" } else { 'all mutants killed' })"
exit $(if ($failed) { 1 } else { 0 })

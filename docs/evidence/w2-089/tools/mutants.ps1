# w2-089 D2: mutation check of tst_device_status (both CTest entries) and tst_ms300_alarm_name.
# Copied from docs\evidence\w2-087\tools\mutants.ps1 (M1..M7 unchanged; patterns now matched with the
# file's own line endings) plus M8 / M9 for the w2-089 name. Each mutant changes one rule in the source,
# rebuilds the Core tests (build\w2-089-run\core-tests, configured by run-tests-w2089.bat core) and runs
# ctest -R "[em]_[sn][ta]" (= tst_device_status, tst_device_status_simulator_profile, tst_ms300_alarm_name;
# a character class instead of an alternation because the argument passes through cmd); the mutant must
# be KILLED (ctest exit != 0). The source file is restored from a byte backup after every mutant (and
# touched, so the next build sees it), and the final run must pass again.
# The new name is built from its code points (U+5FAA U+74B0 U+6CF5 U+8B8A U+983B U+5668), so this file
# needs no BOM for Windows PowerShell 5.1.
# Usage (taidaflow folder): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-089\tools\mutants.ps1 [-Out build\w2-089-run\mutants.txt]
# Exit 0 = baseline passed, every mutant KILLED (except M7, documented as an expected survivor on a PC
# without serial port), final passed, sources byte-identical to the start.
param([string]$Out = 'build\w2-089-run\mutants.txt')
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
Set-Location $root
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
Set-Content -Encoding utf8 $Out "=== w2-089 mutants $(Get-Date -Format o) HEAD $(git rev-parse --short HEAD)"
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Write-Output $line; Add-Content -Encoding utf8 $Out $line }
$enc = New-Object System.Text.UTF8Encoding($false)
$newName = -join ([char[]](0x5FAA, 0x74B0, 0x6CF5, 0x8B8A, 0x983B, 0x5668))
function RunTests([string]$tag) {
    while (@(Get-Process cl, clang, clang++, wasm-ld, ninja -ErrorAction SilentlyContinue).Count) { Start-Sleep -Seconds 10 }
    $log = Join-Path (Split-Path -Parent $Out) "mutant-$tag.log"
    & cmd /c "docs\evidence\w2-089\tools\run-tests-w2089.bat core -R [em]_[sn][ta]" *> $log
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
       old = "    m_deviceStatus->stop();      // w2-087: empty map first - the disconnects below are not reported`n    m_pollTimer.stop();`n    m_ms300FaultReader.stop();`n    m_modbus.disconnectAll();";
       new = "    m_pollTimer.stop();`n    m_ms300FaultReader.stop();`n    m_modbus.disconnectAll();`n    m_deviceStatus->stop();" },
    # M7 only differs when a serial port really opens (see w2-087): expected to survive on this PC.
    @{ id = 'M7-offline-on-connecting'; file = 'Core\Ms300FaultReader.cpp'; expectSurvive = $true;
       old = "        if (connected || state == QModbusDevice::UnconnectedState)`n            emit portOpenChanged(connected);";
       new = "        emit portOpenChanged(connected);" },
    # w2-089: the banner name of the MS300 back to the model.
    @{ id = 'M8-banner-name-MS300'; file = 'Core\DeviceStatusPublisher.cpp';
       old = "const QString kMs300Name = QStringLiteral(`"$newName`");";
       new = "const QString kMs300Name = QStringLiteral(`"MS3`" `"00`");" },
    # w2-089: the alarm record name back to the model (written so the literal check alone does not see it).
    @{ id = 'M9-alarm-name-MS300'; file = 'Core\manager.cpp';
       old = "        saveAlarm(QStringLiteral(`"$newName`"), message, status);";
       new = "        saveAlarm(QStringLiteral(`"MS3`" `"00`"), message, status);" },
    # w2-089: an alarm row of an earlier run rewritten to the new name (old rows must stay as stored).
    @{ id = 'M10-old-rows-renamed'; file = 'Core\manager.cpp';
       old = "        saveAlarm(QStringLiteral(`"$newName`"), message, status);";
       new = "        saveAlarm(QStringLiteral(`"$newName`"), message, status);`n        if (m_sql) { QJsonArray h; QString e; m_sql->getAlarmHistory(0, QDateTime::currentSecsSinceEpoch() + 60, &h, &e); for (const QJsonValue &v : std::as_const(h)) { QJsonObject o = v.toObject(); QJsonObject r = QJsonDocument::fromJson(o.value(QStringLiteral(`"reason`")).toString().toUtf8()).object(); if (r.value(QStringLiteral(`"sensor`")).toString() == QStringLiteral(`"MS3`" `"00`")) { r.insert(QStringLiteral(`"sensor`"), QStringLiteral(`"$newName`")); m_sql->updateAlarmReason(QDateTime::fromSecsSinceEpoch(qint64(o.value(QStringLiteral(`"occurrence_time`")).toDouble())), qint64(o.value(QStringLiteral(`"id`")).toDouble()), QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)), &e); } } }" }
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
    $old = $m.old; $new = $m.new
    if ($text.Contains("`r`n")) {   # the file's own line endings
        $old = $old.Replace("`r`n", "`n").Replace("`n", "`r`n"); $new = $new.Replace("`r`n", "`n").Replace("`n", "`r`n")
    }
    if ($text.IndexOf($old) -lt 0) { Say "$($m.id): pattern not found in $($m.file) - ERROR"; $failed++; continue }
    [System.IO.File]::WriteAllText($path, $text.Replace($old, $new), $enc)
    $rc = RunTests $m.id
    [System.IO.File]::WriteAllBytes($path, $backup[$m.file])
    (Get-Item $path).LastWriteTime = Get-Date
    $mlog = @(Get-Content (Join-Path (Split-Path -Parent $Out) "mutant-$($m.id).log") -Encoding utf8)
    # A mutant that does not compile proves nothing: counted as a problem, not as killed.
    $compileError = @($mlog | Where-Object { $_ -match 'error C\d+|^FAILED: |LNK\d+' })
    $killed = $rc -ne 0 -and $compileError.Count -eq 0
    $why = @($mlog | Where-Object { $_ -match '^FAIL!' } | Select-Object -First 2) -join ' / '
    $note = if ($m.expectSurvive) { ' (expected survivor: needs a serial port that opens - none on this PC)' } else { '' }
    $verdict = if ($compileError.Count) { "COMPILE ERROR (not counted as killed): $($compileError[0])" } elseif ($killed) { 'KILLED' } else { 'SURVIVED' }
    Say "$($m.id) ($($m.file)): ctest exit=$rc -> $verdict$note $why"
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
Say "=== $(if ($failed) { "$failed problem(s)" } else { 'all mutants killed (M7 expected survivor)' })"
exit $(if ($failed) { 1 } else { 0 })

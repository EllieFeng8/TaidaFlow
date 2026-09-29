# w2-072 D6: analyses quiet.log / full.log of the offline live run (live-run.ps1 -Mode offline).
#   * quiet log: warning lines of the offline device ADAM-6217 B (127.0.0.210) <= 4, MS300 (COM2 missing) <= 4;
#   * full log: reconnect attempts of 127.0.0.210 at least 3 s apart; MS300 connection attempts >= ~3 s apart;
#   * the other four ADAM modules (127.0.0.201/202/204/205) were read all the time and have no warning line.
# Before (review evidence, the former code): docs\evidence\w2-057\10-live-ps1\app.log - quoted in the output.
param([Parameter(Mandatory)][string]$Dir, [int]$RunSec = 130)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$summary = Join-Path $Dir 'summary.txt'
$failed = 0
function Note([string]$m) { Add-Content -Path $summary -Value $m -Encoding utf8; [Console]::Out.WriteLine($m) }
function Check([bool]$ok, [string]$what) { if ($ok) { Note "PASS  $what" } else { Note "FAIL  $what"; $script:failed++ } }
function Stamp([string]$line) {
    if ($line.Length -ge 23) {
        $t = [datetime]::MinValue
        if ([datetime]::TryParseExact($line.Substring(0, 23), 'yyyy-MM-dd HH:mm:ss.fff', [Globalization.CultureInfo]::InvariantCulture, 'None', [ref]$t)) { return $t }
    }
    return $null
}

$quiet = @(Get-Content -LiteralPath (Join-Path $Dir 'quiet.log') -Encoding utf8)
$full = @(Get-Content -LiteralPath (Join-Path $Dir 'full.log') -Encoding utf8)
Note ("--- analyse-offline: quiet.log {0} line(s), full.log {1} line(s)" -f $quiet.Count, $full.Count)
$first = Stamp $full[0]; $last = Stamp $full[$full.Count - 1]
Note ("full log covers {0:HH:mm:ss} .. {1:HH:mm:ss} = {2:F0} s" -f $first, $last, ($last - $first).TotalSeconds)
Check (($last - $first).TotalSeconds -ge 120) 'the app ran >= 120 s (full log span)'

$offQuiet = @($quiet | Where-Object { $_ -match '127\.0\.0\.210' })
Note "quiet log lines of ADAM-6217 B (127.0.0.210):"
$offQuiet | ForEach-Object { Note "    $_" }
Check ($offQuiet.Count -le 4) ("offline device warnings in the quiet log: {0} (<= 4)" -f $offQuiet.Count)

$msQuiet = @($quiet | Where-Object { $_ -match '\[MS300\]' })
Note "quiet log lines of MS300:"
$msQuiet | ForEach-Object { Note "    $_" }
Check ($msQuiet.Count -le 4) ("MS300 warnings in the quiet log: {0} (<= 4)" -f $msQuiet.Count)

$attempts = @($full | Where-Object { $_ -match '127\.0\.0\.210\) reconnect attempt #' } | ForEach-Object { Stamp $_ })
$gaps = @(); for ($i = 1; $i -lt $attempts.Count; $i++) { $gaps += ($attempts[$i] - $attempts[$i - 1]).TotalSeconds }
$minGap = if ($gaps.Count) { ($gaps | Measure-Object -Minimum).Minimum } else { 0 }
$ends = @($full | Where-Object { $_ -match '127\.0\.0\.210\) disconnected' } | ForEach-Object { Stamp $_ })
# delay from the end of an attempt to the start of the next one (the 3 s reconnect timer)
$delays = @()
foreach ($a in $attempts) {
    $prev = @($ends | Where-Object { $_ -le $a } | Sort-Object)
    if ($prev.Count) { $delays += ($a - $prev[$prev.Count - 1]).TotalSeconds }
}
$minDelay = if ($delays.Count) { ($delays | Measure-Object -Minimum).Minimum } else { 0 }
Note ("reconnect attempts of 127.0.0.210 in the full log: {0}; gaps between attempts {1} s; delay after the previous attempt ended {2} s" -f `
      $attempts.Count, (($gaps | ForEach-Object { $_.ToString('F1') }) -join ','), (($delays | ForEach-Object { $_.ToString('F1') }) -join ','))
Check ($attempts.Count -ge 2 -and $minGap -ge 3.0) ("reconnect attempts at least 3 s apart (min gap {0:F2} s)" -f $minGap)
Check ($delays.Count -ge 1 -and $minDelay -ge 2.95) ("each attempt starts >= 3 s after the previous one ended (min {0:F2} s)" -f $minDelay)

$msAttempts = @($full | Where-Object { $_ -match '\[MS300\] \S+ connection attempt #' } | ForEach-Object { Stamp $_ })
$msGaps = @(); for ($i = 1; $i -lt $msAttempts.Count; $i++) { $msGaps += ($msAttempts[$i] - $msAttempts[$i - 1]).TotalSeconds }
$msMin = if ($msGaps.Count) { ($msGaps | Measure-Object -Minimum).Minimum } else { 0 }
Note ("MS300 connection attempts (logged, #2..): {0}; gaps {1} s" -f $msAttempts.Count, (($msGaps | ForEach-Object { $_.ToString('F1') }) -join ','))
Check ($msAttempts.Count -ge 2 -and $msMin -ge 2.9) ("MS300 connection attempts at least ~3 s apart (min {0:F2} s; poll timer 1 s)" -f $msMin)

foreach ($h in '201', '202', '204', '205') {
    $reads = @($full | Where-Object { $_ -match "\[Modbus\]\[Read\] device=\S+( \S+)? \(127\.0\.0\.$h\)" -or $_ -match "\(127\.0\.0\.$h\).*\[Modbus\]\[Read\]" })
    $readsAny = @($full | Where-Object { $_ -match "\[Modbus\]\[Read\]" -and $_ -match "127\.0\.0\.$h" })
    $warn = @($quiet | Where-Object { $_ -match "127\.0\.0\.$h" })
    $conn = @($full | Where-Object { $_ -match "127\.0\.0\.$h\) connected" })
    Check ($conn.Count -ge 1 -and $warn.Count -eq 0) ("127.0.0.$h connected ({0}x), [Modbus][Read] lines {1}, warnings in the quiet log {2}" -f $conn.Count, $readsAny.Count, $warn.Count)
}
# The ADAM-6224 / 6022 / 6256 reads are logged per point; 6217 A (202) is an input module: its values
# appear as [Modbus][Read] lines too. Rows saved every second = polling of the four modules went on.

$before = Join-Path $root 'docs\evidence\w2-057\10-live-ps1\app.log'
if (Test-Path $before) {
    $b = @(Get-Content -LiteralPath $before -Encoding utf8)
    $bn = @($b | Where-Object { $_ -match 'not connected' }).Count
    $bm = @($b | Where-Object { $_ -match '\[MS300\]' }).Count
    Note ("before (review evidence docs\evidence\w2-057\10-live-ps1\app.log, former code, devices offline): {0} of {1} lines are 'Device is not connected; request was not sent.', {2} are [MS300] lines" -f $bn, $b.Count, $bm)
}
Note ("--- analyse-offline: {0} failed check(s)" -f $failed)
exit $failed

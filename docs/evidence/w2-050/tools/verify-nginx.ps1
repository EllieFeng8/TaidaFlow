# w2-050 live check of the nginx web front end (port 8123) and the nginx-served downloads.
# No UI test: no browser, no screenshots. Every HTTP request is made with curl.exe; results are
# judged by status codes / headers / hashes / exit codes and written to docs\evidence\w2-050\live\.
#
# Safety: the app is started ONLY through scripts\run-desktop.ps1 -DeviceProfile simulator
# (scripts\safety_probe.ps1 must say SAFE; working directory build\runtime-cwd; probe record in
# docs\evidence\w2-050\safety-probe.log). The simulator is started through scripts\run-simulator.ps1.
# Only processes started here are closed (the app with WM_CLOSE). nginx is started / stopped only
# through scripts\nginx-start.ps1 / nginx-stop.ps1, except the one "foreign" nginx this script starts
# itself for the symbolic link experiment (closed with nginx -s quit on its own prefix). No network,
# firewall or system setting is changed; nothing is registered as a service.
#
# Test fixtures (dev-only, stated in the report; none of them is produced by the app):
#   * build\runtime-cwd\exports: one large CSV (about 1 GB, oldest mtime) + 18 small CSVs with valid
#     export names; a junction named like an export file (created and removed during the run);
#   * secret files outside the web root / export folder for the traversal attempts;
#   * build\w2-050-junction\: a web root with a junction, for the link experiment.
# The exports checked in D7 (e) are produced by the app itself (history export requested over the
# Proxy Mirror by build\w2-050-mirror-client\mirror_export_client.exe, like the web page does).
#
# Usage: powershell -ExecutionPolicy Bypass -File docs\evidence\w2-050\tools\verify-nginx.ps1
# Exit 0 = all checks passed; 1 = a check failed; 2 = not built / tools missing; 3 = ports busy,
# simulator not started or safety probe not SAFE (app not started).
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$ev    = Split-Path -Parent $tools                        # docs\evidence\w2-050
$root  = (Resolve-Path (Join-Path $ev '..\..\..')).Path   # taidaflow
$exe   = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$webDeployed = Join-Path $root 'build\desktop\web'
$devWeb = Join-Path $root 'build\wasm-release'
$cwd = Join-Path $root 'build\runtime-cwd'
$exportsDir = Join-Path $cwd 'exports'
$client = Join-Path $root 'build\w2-050-mirror-client\mirror_export_client.exe'
$py = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'
$dl = Join-Path $root 'build\w2-050-downloads'
$jt = Join-Path $root 'build\w2-050-junction'
$scripts = Join-Path $root 'scripts'
$out   = Join-Path $ev 'live'
New-Item -ItemType Directory -Force $out, $dl | Out-Null
Get-ChildItem $out -File | Remove-Item -Force
Get-ChildItem $dl -File | Remove-Item -Force
$fail = New-Object System.Collections.Generic.List[string]
$app = $null
$sim = $null
$foreign = $null
$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;" + $env:PATH

# Console output goes straight to stdout (not the pipeline), so functions return only their values.
function Note($m) {
    [Console]::Out.WriteLine($m)
    for ($i = 0; $i -lt 20; $i++) {       # a reader (editor, tail) may hold the file for a moment
        try { Add-Content -Path (Join-Path $out 'summary.txt') -Value $m -Encoding utf8; return } catch { Start-Sleep -Milliseconds 100 }
    }
}
function Check($ok, $what) {
    if ($ok) { Note "  PASS $what" } else { Note "  FAIL $what"; $fail.Add($what) }
}
function Listeners([int[]]$ports = @(502, 8123, 8124, 8125, 18125)) {
    @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
}
function Sha($path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash }
function Header($text, $name) {
    $v = ''
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match ('^' + [regex]::Escape($name) + ':\s*(.*)$')) { $v = $Matches[1].Trim() }
    }
    return $v
}
$script:reqNo = 0
function Req($label, $url, [string[]]$extra = @(), [switch]$Keep) {
    $script:reqNo++
    $tag = '{0:D3}-{1}' -f $script:reqNo, ($label -replace '[^A-Za-z0-9_.-]', '_')
    $hdr = Join-Path $out "$tag.headers.txt"
    $body = Join-Path $out "$tag.body"
    $w = & curl.exe -s --path-as-is --max-time 120 -D $hdr -o $body -w '%{http_code} %{size_download} %{time_total}' @extra $url
    $rc = $LASTEXITCODE
    $parts = "$w" -split ' '
    $h = if (Test-Path $hdr) { Get-Content -Raw $hdr } else { '' }
    $r = [pscustomobject]@{ label = $label; url = $url; code = $parts[0]; size = [int64]$parts[1]; time = $parts[2];
                            headers = $h; body = $body; curl = $rc }
    Note ("  {0} {1} -> {2} ({3} bytes, {4} s, curl {5}) {6}" -f $label, $url, $r.code, $r.size, $r.time, $rc, (($extra -join ' ') -replace '\s+', ' '))
    return $r
}
function SmallBody($r) { if ((Test-Path $r.body) -and (Get-Item $r.body).Length -lt 65536) { return [IO.File]::ReadAllText($r.body) } else { return '' } }
function NoLeak($r) { $b = SmallBody $r; return -not ($b -match 'SECRET' -or $b -match '(?<![A-Za-z])[A-Za-z]:[\\/]' -or $b -match 'repo' -or $b -match 'nginx/\d') }
function Gunzip($path) {
    $in = [System.IO.File]::OpenRead($path)
    try {
        $gz = New-Object System.IO.Compression.GZipStream($in, [System.IO.Compression.CompressionMode]::Decompress)
        $ms = New-Object System.IO.MemoryStream
        $gz.CopyTo($ms); $gz.Dispose()
        $ms.Position = 0
        $sha = [System.Security.Cryptography.SHA256]::Create()
        return ([System.BitConverter]::ToString($sha.ComputeHash($ms)) -replace '-', '')
    } finally { $in.Dispose() }
}
function ShaOfRange($path, [int64]$offset, [int64]$count) {
    $fs = [System.IO.File]::OpenRead($path)
    try {
        $buf = New-Object byte[] $count
        $null = $fs.Seek($offset, 'Begin')
        $read = 0
        while ($read -lt $count) { $n = $fs.Read($buf, $read, $count - $read); if ($n -le 0) { break }; $read += $n }
        $sha = [System.Security.Cryptography.SHA256]::Create()
        return ([System.BitConverter]::ToString($sha.ComputeHash($buf, 0, $read)) -replace '-', '')
    } finally { $fs.Dispose() }
}
function NginxPids {
    $st = Join-Path $root 'build\nginx\taidaflow-nginx.json'
    if (-not (Test-Path $st)) { return @() }
    $m = [int]((Get-Content -Raw $st | ConvertFrom-Json).pid)
    return @($m) + @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$m" -ErrorAction SilentlyContinue | ForEach-Object { [int]$_.ProcessId })
}
function Mem([int[]]$ids) {
    $ws = [int64]0; $pm = [int64]0
    foreach ($id in $ids) { $p = Get-Process -Id $id -ErrorAction SilentlyContinue; if ($p) { $ws += $p.WorkingSet64; $pm += $p.PrivateMemorySize64 } }
    return [pscustomobject]@{ ws = $ws; pm = $pm }
}
# curl in its own process while the memory of nginx (master + worker) and the app is sampled.
function SampledCurl($label, [string[]]$curlArgs) {
    $npids = NginxPids
    $apid = if ($script:app) { $script:app.Id } else { 0 }
    $n0 = Mem $npids; $a0 = Mem @($apid)
    $nPeakWs = $n0.ws; $nPeakPm = $n0.pm; $aPeakWs = $a0.ws; $aPeakPm = $a0.pm; $samples = 0
    $wfile = Join-Path $out ("{0}-curl-w.txt" -f ($label -replace '[^A-Za-z0-9_.-]', '_'))
    $argLine = ($curlArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $cp = Start-Process curl.exe -ArgumentList $argLine -PassThru -WindowStyle Hidden -RedirectStandardOutput $wfile
    $null = $cp.Handle
    while (-not $cp.HasExited) {
        Start-Sleep -Milliseconds 200
        $n = Mem $npids; $a = Mem @($apid); $samples++
        if ($n.ws -gt $nPeakWs) { $nPeakWs = $n.ws }; if ($n.pm -gt $nPeakPm) { $nPeakPm = $n.pm }
        if ($a.ws -gt $aPeakWs) { $aPeakWs = $a.ws }; if ($a.pm -gt $aPeakPm) { $aPeakPm = $a.pm }
    }
    $cp.WaitForExit()
    $w = (Get-Content -Raw $wfile).Trim()
    $mb = { param($v) '{0:N1} MB' -f ($v / 1MB) }
    Note ("  {0}: curl exit {1}, -w '{2}', {3} samples; nginx pids {4}: WS {5} -> peak {6}, private {7} -> peak {8}; app pid {9}: WS {10} -> peak {11}, private {12} -> peak {13}" -f
          $label, $cp.ExitCode, $w, $samples, ($npids -join ','), (& $mb $n0.ws), (& $mb $nPeakWs), (& $mb $n0.pm), (& $mb $nPeakPm),
          $apid, (& $mb $a0.ws), (& $mb $aPeakWs), (& $mb $a0.pm), (& $mb $aPeakPm))
    return [pscustomobject]@{ exit = $cp.ExitCode; w = $w; samples = $samples;
                              nGrowWs = $nPeakWs - $n0.ws; nGrowPm = $nPeakPm - $n0.pm; aGrowWs = $aPeakWs - $a0.ws; aGrowPm = $aPeakPm - $a0.pm;
                              nPeakWs = $nPeakWs }
}
function CloseApp {
    if ($script:app -and -not $script:app.HasExited) {
        $null = $script:app.CloseMainWindow()
        if (-not $script:app.WaitForExit(20000)) { Note '  app did not exit on WM_CLOSE within 20 s - stopping OUR instance'; Stop-Process -Id $script:app.Id -Force }
        Note "  app pid $($script:app.Id) exit code $($script:app.ExitCode)"
    }
    $script:app = $null
}
function CloseSim {
    if ($script:sim -and -not $script:sim.HasExited) {
        $null = $script:sim.CloseMainWindow()
        if (-not $script:sim.WaitForExit(10000)) { Stop-Process -Id $script:sim.Id -Force }
        Note "simulator pid $($script:sim.Id) closed"
    }
    $script:sim = $null
}
# Native program with stdout + stderr in a file (Start-Process, no PowerShell error records).
function RunLogged([string]$exePath, [string[]]$a, [string]$outFile) {
    $argLine = ($a | ForEach-Object { if ($_ -eq '' -or $_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath $exePath -ArgumentList $argLine -PassThru -NoNewWindow -RedirectStandardOutput $outFile -RedirectStandardError "$outFile.stderr"
    $null = $p.Handle
    $p.WaitForExit()
    if ((Test-Path "$outFile.stderr") -and (Get-Item "$outFile.stderr").Length -gt 0) { Add-Content -Path $outFile -Value (Get-Content "$outFile.stderr") }
    Remove-Item "$outFile.stderr" -Force -ErrorAction SilentlyContinue
    return $p.ExitCode
}
function StopForeign {
    if ($script:foreign -and -not $script:foreign.p.HasExited) {
        $null = RunLogged $script:foreign.exe @('-p', $script:foreign.prefix, '-c', $script:foreign.conf, '-s', 'quit') (Join-Path $out 'foreign-nginx-quit.txt')
        if (-not $script:foreign.p.WaitForExit(15000)) { Note '  foreign nginx did not quit - stopping it (started by this script)'; Stop-Process -Id $script:foreign.p.Id -Force }
        Note "  foreign nginx pid $($script:foreign.p.Id) (started by this script) exited"
    }
    $script:foreign = $null
}
function RemoveJunction($path) {
    if (Test-Path -LiteralPath $path) {
        $d = New-Object System.IO.DirectoryInfo $path
        if ($d.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $d.Delete() }   # removes the link only
    }
}
function NginxScript($name, [string[]]$scriptArgs, $outFile) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $scripts $name)) + $scriptArgs
    $rc = RunLogged 'powershell.exe' $argList $outFile
    Note ("  {0} {1} -> exit {2}" -f $name, ($scriptArgs -join ' '), $rc)
    Get-Content $outFile | ForEach-Object { Note "    | $_" }
    return $rc
}
# Runs a launcher script with its stdout in a file (never a pipe: the started program could hold
# a pipe open), waits for the launcher only, returns its exit code.
function RunLauncher($script, [string[]]$scriptArgs, $outFile) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"") + $scriptArgs
    $runner = Start-Process powershell -PassThru -WindowStyle Hidden -RedirectStandardOutput $outFile `
                  -RedirectStandardError "$outFile.stderr" -ArgumentList $argList
    $null = $runner.Handle
    $runner.WaitForExit()
    return $runner.ExitCode
}
function Launch($scenario) {
    $log = Join-Path $out "app-$scenario.log"
    $runTxt = Join-Path $out "run-desktop-$scenario.txt"
    $rc = RunLauncher (Join-Path $scripts 'run-desktop.ps1') @('-DeviceProfile', 'simulator', '-Label', "`"w2-050 $scenario`"",
            '-ProbeLog', "`"$(Join-Path $ev 'safety-probe.log')`"", '-LogFile', "`"$log`"") $runTxt
    $m = Select-String -CaseSensitive -Path $runTxt -Pattern '^PID=(\d+)' | Select-Object -First 1
    if ($m) { $script:app = Get-Process -Id ([int]$m.Matches[0].Groups[1].Value); $null = $script:app.Handle }
    Note ("run-desktop ($scenario): " + ((Get-Content $runTxt | Select-Object -Last 3) -join ' | ') + " (exit $rc)")
    if ($rc -ne 0 -or -not $script:app) { return $false }
    $t0 = Get-Date
    do {
        Start-Sleep -Milliseconds 500
        $up = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $script:app.Id -and $_.LocalPort -in 8124, 8125 })
    } while ($up.Count -lt 2 -and ((Get-Date) - $t0).TotalSeconds -lt 40 -and -not $script:app.HasExited)
    Start-Sleep -Seconds 3
    return $up.Count -ge 2
}
function AppLog($scenario) { Get-Content (Join-Path $out "app-$scenario.log") -Encoding utf8 }
function MirrorExport($label, $wsHost, $session) {
    $o = Join-Path $out "mirror-export-$label.txt"
    $from = [double]([DateTimeOffset]::new(2026, 9, 1, 0, 0, 0, [TimeSpan]::FromHours(8)).ToUnixTimeMilliseconds())
    $to = [double]([DateTimeOffset]::Now.ToUnixTimeMilliseconds())
    $rc = RunLogged $client @('--url', "ws://${wsHost}:8125/mirror", '--origin', "http://${wsHost}:8123", '--session', $session,
                              '--from-ms', ('{0:F0}' -f $from), '--to-ms', ('{0:F0}' -f $to), '--timeout-sec', '120') $o
    Get-Content $o -Encoding UTF8 | ForEach-Object { Note "    | $_" }
    $m = Select-String -Path $o -Pattern 'RESULT state=(\S+) fileName=(\S*) url=(\S*) downloadPort=(\S*)' | Select-Object -First 1
    $r = [pscustomobject]@{ rc = $rc; state = ''; fileName = ''; url = ''; port = '' }
    if ($m) { $g = $m.Matches[0].Groups; $r.state = $g[1].Value; $r.fileName = $g[2].Value; $r.url = $g[3].Value; $r.port = $g[4].Value }
    Note ("  mirror export ($label, session $session via ws://${wsHost}:8125): exit $rc, state $($r.state), file $($r.fileName), url $($r.url), downloadPort $($r.port)")
    return $r
}
function Cleanup {
    StopForeign
    if (Test-Path (Join-Path $root 'build\nginx\taidaflow-nginx.json')) {
        $null = NginxScript 'nginx-stop.ps1' @() (Join-Path $out 'nginx-stop-cleanup.txt')
    }
    CloseApp; CloseSim
    Remove-Item Env:\TAIDAFLOW_DOWNLOAD_PORT -ErrorAction SilentlyContinue
    RemoveJunction (Join-Path $exportsDir 'w2050junction_20260101_000000.csv')
    RemoveJunction (Join-Path $jt 'web\linked')
    RemoveJunction (Join-Path $jt 'exports\w2050junction_20260101_000000.csv')
    RemoveJunction (Join-Path $jt 'exe\web\linked')
}
trap {
    Note "ERROR: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
    Cleanup
    exit 1
}

# ---------------------------------------------------------------- preconditions
# Leftovers of an interrupted earlier run of this script (only its own w2050* fixtures).
foreach ($f in @(Get-ChildItem $exportsDir -Filter 'w2050*' -Force -ErrorAction SilentlyContinue)) {
    if ($f.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { RemoveJunction $f.FullName } else { Remove-Item -LiteralPath $f.FullName -Force }
}
Remove-Item -LiteralPath (Join-Path $root 'build\desktop\w2050-secret.js'), (Join-Path $cwd 'w2050-secret.txt'), (Join-Path $cwd 'w2050secret_20260101_000000.csv'),
    (Join-Path $webDeployed '.w2050hidden.js'), (Join-Path $webDeployed 'w2050-notweb.txt') -Force -ErrorAction SilentlyContinue
foreach ($f in $exe, $client, $py) { if (-not (Test-Path $f)) { Note "missing: $f"; exit 2 } }
if (Get-Process TaidaFlowApp -ErrorAction SilentlyContinue) { Note 'TaidaFlowApp already running (not ours) - not touching it'; exit 3 }
if (Get-Process Adam60xxSimulator -ErrorAction SilentlyContinue) { Note 'Adam60xxSimulator already running (not ours) - not touching it'; exit 3 }
$preNginx = @(Get-Process nginx -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
Note ("nginx processes before the run (never touched): " + $(if ($preNginx.Count) { $preNginx -join ',' } else { '<none>' }))
$deadline = (Get-Date).AddMinutes(10)
while ((Listeners).Count -gt 0) {
    $l = Listeners | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid=$($_.OwningProcess)" }
    Note ("ports busy: " + ($l -join ', ') + " - re-check in 30 s (owner not stopped)")
    if ((Get-Date) -gt $deadline) { Note 'ports still busy after 10 min'; exit 3 }
    Start-Sleep -Seconds 30
}
$lan = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
         Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
         Sort-Object InterfaceMetric | ForEach-Object { $_.IPAddress })
Note ("local IPv4: " + ($lan -join ', '))
$lanIp = if ($lan.Count) { $lan[0] } else { '' }
if (-not $lanIp) { Note 'no LAN IPv4'; exit 2 }
Note "LAN IPv4 used for the LAN requests: $lanIp"
$hosts = @('127.0.0.1', $lanIp)

# ---------------------------------------------------------------- deploy + nginx -t
Note '=== deploy the web page (scripts\deploy-web.ps1)'
$rc = NginxScript 'deploy-web.ps1' @() (Join-Path $out 'deploy-web.txt')
Check ($rc -eq 0) 'deploy-web.ps1 exit 0'
Note '=== nginx -t (scripts\nginx-start.ps1 -Test)'
$rc = NginxScript 'nginx-start.ps1' @('-Test') (Join-Path $out 'nginx-test.txt')
Check ($rc -eq 0 -and (Select-String -Path (Join-Path $out 'nginx-test.txt') -SimpleMatch 'test is successful' -Quiet)) 'nginx -t: test is successful (exit 0)'

# ---------------------------------------------------------------- symbolic link / junction experiment
Note '=== link experiment: nginx for Windows and a junction in the web root'
if (Test-Path $jt) { Get-ChildItem $jt -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue | ForEach-Object { RemoveJunction $_.FullName }; Remove-Item -LiteralPath $jt -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $jt 'web'), (Join-Path $jt 'outside'), (Join-Path $jt 'exports'), (Join-Path $jt 'runtime\conf'), (Join-Path $jt 'runtime\logs'), (Join-Path $jt 'runtime\temp'), (Join-Path $jt 'exe\web') | Out-Null
Copy-Item (Join-Path $webDeployed 'TaidaFlowApp.html') (Join-Path $jt 'web\TaidaFlowApp.html')
Set-Content -Path (Join-Path $jt 'outside\secret.js') -Value 'SECRET-OUTSIDE-WEBROOT' -Encoding ascii
Set-Content -Path (Join-Path $jt 'outside\w2050junction_20260101_000000.csv') -Value 'SECRET-OUTSIDE-EXPORTS' -Encoding ascii
$null = New-Item -ItemType Junction -Path (Join-Path $jt 'web\linked') -Target (Join-Path $jt 'outside')
Note "  junction $jt\web\linked -> $jt\outside (holds secret.js)"
# A nginx started directly (NOT through nginx-start.ps1, which would refuse) with the same template.
$ngx = (Get-ChildItem 'C:\tools\nginx' -Directory -Filter 'nginx-*' | Sort-Object { [version]$_.Name.Substring(6) } | Select-Object -Last 1).FullName + '\nginx.exe'
$fprefix = ((Join-Path $jt 'runtime') -replace '\\', '/') + '/'
$fconf = (Join-Path $jt 'runtime\conf\taidaflow.conf') -replace '\\', '/'
$tpl = [IO.File]::ReadAllText((Join-Path $root 'deploy\nginx\taidaflow.conf'))
$tpl = $tpl.Replace('@TAIDAFLOW_WEB_ROOT@', ((Join-Path $jt 'web') -replace '\\', '/')).Replace('@TAIDAFLOW_EXPORT_DIR@', ((Join-Path $jt 'exports') -replace '\\', '/'))
[IO.File]::WriteAllText($fconf, $tpl, (New-Object System.Text.UTF8Encoding($false)))
$fp = Start-Process -FilePath $ngx -WorkingDirectory (Join-Path $jt 'runtime') -WindowStyle Hidden -PassThru -ArgumentList @('-p', "`"$fprefix`"", '-c', "`"$fconf`"")
$null = $fp.Handle
$foreign = [pscustomobject]@{ p = $fp; exe = $ngx; prefix = $fprefix; conf = $fconf }
$t0 = Get-Date
while (@(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue).Count -eq 0 -and ((Get-Date) - $t0).TotalSeconds -lt 15) { Start-Sleep -Milliseconds 250 }
Note "  foreign nginx (started directly by this script, pid $($fp.Id)) with web root $jt\web"
$r = Req 'link-nginx-follows' 'http://127.0.0.1:8123/linked/secret.js'
$followed = ($r.code -eq '200' -and (SmallBody $r) -match 'SECRET-OUTSIDE-WEBROOT')
Note ("  RESULT: nginx for Windows {0} the junction (HTTP {1}) -> the start/deploy scripts must refuse such folders" -f $(if ($followed) { 'FOLLOWS' } else { 'does not follow' }), $r.code)
Check ($r.code -ne '') 'link experiment recorded (what nginx does with a junction in the web root)'
$fl = @(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue | ForEach-Object { $_.OwningProcess })
Note "  8123 listener pid(s) of the foreign nginx: $($fl -join ',')"

Note '=== D3: scripts never touch an nginx they did not start; port 8123 busy -> no start'
$rc = NginxScript 'nginx-stop.ps1' @() (Join-Path $out 'nginx-stop-foreign.txt')
Check ($rc -eq 1 -and -not $fp.HasExited -and @(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue).Count -gt 0) 'nginx-stop.ps1 with only a foreign nginx running -> exit 1, foreign nginx still running and listening'
$rc = NginxScript 'nginx-start.ps1' @() (Join-Path $out 'nginx-start-port-busy-nginx.txt')
Check ($rc -eq 4 -and -not $fp.HasExited -and -not (Test-Path (Join-Path $root 'build\nginx\taidaflow-nginx.json'))) 'nginx-start.ps1 while 8123 is held by another nginx -> exit 4, nothing started, other nginx untouched'
Note '=== D6: safety probe with a listener on 8123 (default profile)'
$probeTxt = Join-Path $out 'safety-probe-8123-info.txt'
$prc = RunLogged 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $scripts 'safety_probe.ps1'), '-Reason', 'w2-050 D6: 8123 listener is information only', '-LogFile', (Join-Path $ev 'safety-probe.log')) $probeTxt
Get-Content $probeTxt | ForEach-Object { Note "    | $_" }
Check ($prc -eq 0 -and (Select-String -Path $probeTxt -SimpleMatch 'port 8123 = nginx web front end' -Quiet) -and (Select-String -Path $probeTxt -SimpleMatch 'verdict: SAFE' -Quiet)) "safety probe: 8123 listener reported as information only, verdict SAFE (exit $prc)"
StopForeign
Start-Sleep -Milliseconds 500
Check (@(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue).Count -eq 0) 'foreign nginx closed (by this script): 8123 free'

$bl = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 8123)
$bl.Start()
$rc = NginxScript 'nginx-start.ps1' @() (Join-Path $out 'nginx-start-port-busy-other.txt')
$bl.Stop()
Check ($rc -eq 4 -and @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { $preNginx -notcontains $_.Id }).Count -eq 0) 'nginx-start.ps1 while 8123 is held by another program (a test listener of this script) -> exit 4, no nginx started'

$rc = NginxScript 'nginx-start.ps1' @('-WebRoot', (Join-Path $jt 'web')) (Join-Path $out 'nginx-start-junction-webroot.txt')
Check ($rc -eq 5 -and @(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue).Count -eq 0 -and @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { $preNginx -notcontains $_.Id }).Count -eq 0) 'nginx-start.ps1 -WebRoot <folder with a junction> -> exit 5 (refused), no listener, no nginx'
$null = New-Item -ItemType Junction -Path (Join-Path $jt 'exports\w2050junction_20260101_000000.csv') -Target (Join-Path $jt 'outside')
$rc = NginxScript 'nginx-start.ps1' @('-WebRoot', $webDeployed, '-ExportDir', (Join-Path $jt 'exports')) (Join-Path $out 'nginx-start-junction-exports.txt')
Check ($rc -eq 5 -and @(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue).Count -eq 0) 'nginx-start.ps1 -ExportDir <folder with a junction> -> exit 5 (refused), no listener'
RemoveJunction (Join-Path $jt 'exports\w2050junction_20260101_000000.csv')
# deploy-web.ps1 must not remove (and follow) a junction in an existing web folder.
Set-Content -Path (Join-Path $jt 'exe\web\old.js') -Value 'old' -Encoding ascii
$null = New-Item -ItemType Junction -Path (Join-Path $jt 'exe\web\linked') -Target (Join-Path $jt 'outside')
$rc = NginxScript 'deploy-web.ps1' @('-ExeDir', (Join-Path $jt 'exe')) (Join-Path $out 'deploy-web-junction.txt')
Check ($rc -eq 5 -and (Test-Path (Join-Path $jt 'outside\secret.js')) -and (Test-Path (Join-Path $jt 'exe\web\old.js'))) 'deploy-web.ps1 with a junction in the old web folder -> exit 5, nothing removed, junction target intact'
RemoveJunction (Join-Path $jt 'exe\web\linked')
RemoveJunction (Join-Path $jt 'web\linked')
Check ((Test-Path (Join-Path $jt 'outside\secret.js')) -and -not (Test-Path (Join-Path $jt 'web\linked'))) 'test junctions removed (link only; target files intact)'

# ---------------------------------------------------------------- fixtures
Note '=== fixtures (dev-only)'
New-Item -ItemType Directory -Force $exportsDir | Out-Null
$bigName = 'w2050big_20260101_000000.csv'
$big = Join-Path $exportsDir $bigName
& $py -B (Join-Path $tools 'make_big_csv.py') $big
(Get-Item $big).LastWriteTime = [datetime]'2026-01-01 00:00:00'
$bigLen = (Get-Item $big).Length
$bigSha = Sha $big
Note "  large CSV $big : $bigLen bytes, sha256 $bigSha (mtime 2026-01-01, the oldest file)"
$small = @()
for ($i = 1; $i -le 18; $i++) {
    $n = ('w2050fx{0:D2}_20260102_{1:D6}.csv' -f $i, $i)
    $pth = Join-Path $exportsDir $n
    [IO.File]::WriteAllText($pth, ('"seq","v"' + "`r`n" + (('"{0}","{0}.5"' -f $i) + "`r`n") * 200), (New-Object System.Text.UTF8Encoding($false)))
    (Get-Item $pth).LastWriteTime = ([datetime]'2026-01-02 00:00:00').AddMinutes($i)
    $small += $n
}
Note "  18 small CSVs w2050fx01..18 (mtime 2026-01-02); export folder now holds $(@(Get-ChildItem $exportsDir -Filter *.csv).Count) csv file(s)"
Set-Content -Path (Join-Path $root 'build\desktop\w2050-secret.js') -Value 'SECRET-OUTSIDE-WEBROOT-JS' -Encoding ascii
Set-Content -Path (Join-Path $cwd 'w2050-secret.txt') -Value 'SECRET-OUTSIDE-EXPORTS' -Encoding ascii
Set-Content -Path (Join-Path $cwd 'w2050secret_20260101_000000.csv') -Value 'SECRET-OUTSIDE-EXPORTS-CSV' -Encoding ascii
Set-Content -Path (Join-Path $webDeployed '.w2050hidden.js') -Value 'SECRET-HIDDEN' -Encoding ascii
Set-Content -Path (Join-Path $webDeployed 'w2050-notweb.txt') -Value 'SECRET-NOT-A-WEB-TYPE' -Encoding ascii

# ---------------------------------------------------------------- simulator + app (TAIDAFLOW_DOWNLOAD_PORT=8123)
$simTxt = Join-Path $out 'run-simulator.txt'
$rc = RunLauncher (Join-Path $scripts 'run-simulator.ps1') @('-LogFile', "`"$(Join-Path $out 'simulator.log')`"") $simTxt
Note ("run-simulator: " + ((Get-Content $simTxt) -join ' ') + " (exit $rc)")
$m = Select-String -CaseSensitive -Path $simTxt -Pattern '^SIM_PID=(\d+)' | Select-Object -First 1
if ($m) { $sim = Get-Process -Id ([int]$m.Matches[0].Groups[1].Value); $null = $sim.Handle }
if ($rc -ne 0 -or -not $sim) { Note 'simulator not started'; Cleanup; exit 3 }

Note '=== safety probe (simulator profile) before the desktop launch'
$probeTxt = Join-Path $out 'safety-probe-before-launch.txt'
$prc = RunLogged 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $scripts 'safety_probe.ps1'), '-Reason', 'w2-050 before launch (explicit)', '-DeviceProfile', 'simulator', '-LogFile', (Join-Path $ev 'safety-probe.log')) $probeTxt
Get-Content $probeTxt | ForEach-Object { Note "    | $_" }
Check ($prc -eq 0) "safety probe (simulator profile) SAFE before launch (exit $prc)"
if ($prc -ne 0) { Cleanup; exit 3 }

$env:TAIDAFLOW_DOWNLOAD_PORT = '8123'
Note '=== scenario nginx: app with TAIDAFLOW_DOWNLOAD_PORT=8123 (run-desktop.ps1 -DeviceProfile simulator), then nginx-start.ps1'
if (-not (Launch 'nginx')) { Note 'app not started / 8124+8125 not listening'; Cleanup; exit 3 }
$wl = (AppLog 'nginx') -join "`n"
Check ($wl -match [regex]::Escape('[Export] TAIDAFLOW_DOWNLOAD_PORT=8123 - download links use port 8123 (served by another program, e.g. nginx; /exports on AppHttpServer port 8124 stays available as fallback)')) 'app log: TAIDAFLOW_DOWNLOAD_PORT=8123 -> download links use port 8123'
Check ($wl -match '\[Export\] web export folder .*download links -> port 8123') 'app log: export manager uses port 8123 for the links'
Check ($wl -match [regex]::Escape('[Export] download mount GET /exports/<file> -> ') -and $wl -match 'mounted on AppHttpServer') 'app log: /exports still mounted on AppHttpServer (8124 fallback)'
(AppLog 'nginx') | Where-Object { $_ -match '\[Export\]|\[Web\]|\[AppHttpServer\] (listening|static|downloads)' } | ForEach-Object { Note "    $_" }

$rc = NginxScript 'nginx-start.ps1' @() (Join-Path $out 'nginx-start.txt')
Check ($rc -eq 0) 'nginx-start.ps1 -> exit 0'
$npids = NginxPids
$l8123 = @(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue)
Check ($l8123.Count -ge 1 -and @($l8123 | Where-Object { $_.LocalAddress -ne '0.0.0.0' -or $npids -notcontains $_.OwningProcess }).Count -eq 0) "0.0.0.0:8123 listening, owned by our nginx (pids $($npids -join ','))"

# ---------------------------------------------------------------- web page checks
$wasm = Join-Path $webDeployed 'TaidaFlowApp.wasm'
$wasmLen = (Get-Item $wasm).Length; $gzLen = (Get-Item "$wasm.gz").Length
$wasmSha = Sha $wasm; $gzSha = Sha "$wasm.gz"
Note "deployed TaidaFlowApp.wasm $wasmLen bytes, .gz $gzLen bytes"
foreach ($h in $hosts) {
    $base = "http://${h}:8123"
    Note "--- web page, host $h"
    $r = Req "html-$h" "$base/TaidaFlowApp.html"
    Check ($r.code -eq '200' -and (Header $r.headers 'content-type') -eq 'text/html; charset=utf-8') "$h html 200 text/html; charset=utf-8"
    Check ((Header $r.headers 'cache-control') -eq 'no-cache' -and (Header $r.headers 'etag') -ne '' -and (Header $r.headers 'last-modified') -ne '') "$h html Cache-Control no-cache + ETag + Last-Modified"
    Check ((Header $r.headers 'cross-origin-opener-policy') -eq 'same-origin' -and (Header $r.headers 'cross-origin-embedder-policy') -eq 'require-corp' -and (Header $r.headers 'cross-origin-resource-policy') -eq 'same-origin') "$h html COOP same-origin / COEP require-corp / CORP same-origin"
    Check ((Header $r.headers 'server') -eq 'nginx') "$h Server header without version (server_tokens off)"
    Check ((Sha $r.body) -eq (Sha (Join-Path $webDeployed 'TaidaFlowApp.html'))) "$h html body = deployed file"
    $rg = Req "html-gzip-$h" "$base/TaidaFlowApp.html" @('-H', 'Accept-Encoding: gzip')
    Check ($rg.code -eq '200' -and (Header $rg.headers 'content-encoding') -eq 'gzip' -and $rg.size -eq (Get-Item (Join-Path $webDeployed 'TaidaFlowApp.html.gz')).Length) "$h html gzip variant = deployed .gz"
    $rw = Req "wasm-identity-$h" "$base/TaidaFlowApp.wasm"
    Check ($rw.code -eq '200' -and (Header $rw.headers 'content-type') -eq 'application/wasm') "$h wasm 200 application/wasm"
    Check ($rw.size -eq $wasmLen -and (Sha $rw.body) -eq $wasmSha -and (Header $rw.headers 'content-encoding') -eq '') "$h wasm identity $wasmLen bytes, sha256 equal, no Content-Encoding"
    Check ((Header $rw.headers 'cache-control') -eq 'no-cache' -and (Header $rw.headers 'vary') -eq 'Accept-Encoding') "$h wasm Cache-Control no-cache (revalidate) + Vary: Accept-Encoding"
    $etag = Header $rw.headers 'etag'
    $rz = Req "wasm-gzip-$h" "$base/TaidaFlowApp.wasm" @('-H', 'Accept-Encoding: gzip, deflate, br')
    Check ($rz.code -eq '200' -and (Header $rz.headers 'content-encoding') -eq 'gzip' -and (Header $rz.headers 'content-type') -eq 'application/wasm') "$h wasm gzip: Content-Encoding gzip, application/wasm"
    Check ($rz.size -eq $gzLen -and (Sha $rz.body) -eq $gzSha -and (Header $rz.headers 'vary') -eq 'Accept-Encoding') "$h wasm gzip size $gzLen = deployed .gz (sha256 equal), Vary"
    Check ((Gunzip $rz.body) -eq $wasmSha) "$h wasm gzip body decompresses to the .wasm"
    $etagGz = Header $rz.headers 'etag'
    $qEtag = $etag -replace '"', '\"'; $qEtagGz = $etagGz -replace '"', '\"'
    $r304 = Req "wasm-304-$h" "$base/TaidaFlowApp.wasm" @('-H', "If-None-Match: $qEtag")
    Check ($r304.code -eq '304' -and $r304.size -eq 0) "$h wasm second request with ETag -> 304"
    $r304g = Req "wasm-gzip-304-$h" "$base/TaidaFlowApp.wasm" @('-H', 'Accept-Encoding: gzip', '-H', "If-None-Match: $qEtagGz")
    Check ($r304g.code -eq '304' -and $r304g.size -eq 0) "$h wasm gzip second request with its ETag -> 304"
    $rims = Req "html-ims-$h" "$base/TaidaFlowApp.html" @('-H', ('If-Modified-Since: ' + (Header $r.headers 'last-modified')))
    Check ($rims.code -eq '304') "$h html If-Modified-Since -> 304"
    $rhead = Req "wasm-head-$h" "$base/TaidaFlowApp.wasm" @('-I')
    Check ($rhead.code -eq '200' -and [int64](Header $rhead.headers 'content-length') -eq $wasmLen -and $rhead.size -eq 0) "$h HEAD wasm 200, Content-Length $wasmLen"
    foreach ($f in @(@('TaidaFlowApp.js', 'text/javascript; charset=utf-8'), @('qtloader.js', 'text/javascript; charset=utf-8'), @('qtlogo.svg', 'image/svg+xml'))) {
        $rf = Req "$($f[0])-$h" "$base/$($f[0])"
        Check ($rf.code -eq '200' -and (Header $rf.headers 'content-type') -eq $f[1] -and (Sha $rf.body) -eq (Sha (Join-Path $webDeployed $f[0]))) "$h $($f[0]) 200 $($f[1])"
    }
    $rr = Req "root-$h" "$base/"
    Check ($rr.code -eq '302' -and (Header $rr.headers 'location') -eq '/TaidaFlowApp.html') "$h / -> 302 Location: /TaidaFlowApp.html"
    $attacks = @('/../w2050-secret.js', '/%2e%2e/w2050-secret.js', '/%2E%2E%2Fw2050-secret.js', '/..%5Cw2050-secret.js', '/..\w2050-secret.js',
                 '/..%252Fw2050-secret.js', '/exports/../../w2050-secret.js', '/CMakeCache.txt', '/build.ninja', '/../CMakeCache.txt',
                 '/%2e%2e/build.ninja', '/w2050-notweb.txt', '/.w2050hidden.js', '/TaidaFlowApp.wasm.gz', '/TaidaFlowApp.htm',
                 '/TaidaFlowApp.html::$DATA', '/NUL', '/NUL.js', '/con.html', '/QTLOAD~1.JS',
                 '/C:%5CWindows%5Cwin.ini', '/C:/Windows/win.ini', '/%5C%5C127.0.0.1%5Cc$%5CWindows%5Cwin.ini', '/web/', '/nope.js')
    foreach ($a in $attacks) {
        $ra = Req "attack-$h" "$base$a"
        Check (($ra.code -in '400', '403', '404') -and (NoLeak $ra)) "$h $a -> $($ra.code), no secret / path / version in the body"
    }
    # nginx for Windows drops a trailing '.' / ' ' (Windows file name rules): the same page, nothing else.
    foreach ($a in '/TaidaFlowApp.html.', '/TaidaFlowApp.html%20', '/qtloader.js.', '/w2050-notweb.txt.', '/..%2Fw2050-secret.js.') {
        $ra = Req "trailing-$h" "$base$a"
        $same = $false
        foreach ($f in 'TaidaFlowApp.html', 'qtloader.js') { if ($ra.code -eq '200' -and (Sha $ra.body) -eq (Sha (Join-Path $webDeployed $f))) { $same = $true } }
        Check ((($ra.code -in '400', '403', '404') -or $same) -and (NoLeak $ra)) "$h $a -> $($ra.code) (200 only as the same deployed web file; nothing else reachable)"
    }
    $rp = Req "post-html-$h" "$base/TaidaFlowApp.html" @('-X', 'POST')
    Check ($rp.code -eq '405') "$h POST /TaidaFlowApp.html -> 405"
}
Note '--- reload with -WebRoot build\wasm-release (a build folder with CMakeCache.txt / build.ninja)'
$rc = NginxScript 'nginx-web.ps1' @('-Action', 'reload', '-WebRoot', $devWeb) (Join-Path $out 'nginx-reload-wasm-release.txt')
Check ($rc -eq 0) 'nginx-web.ps1 -Action reload -WebRoot build\wasm-release -> exit 0'
Start-Sleep -Seconds 1
foreach ($p in '/CMakeCache.txt', '/build.ninja', '/cmake_install.cmake', '/CMakeFiles/', '/TaidaFlowApp_autogen/') {
    $r = Req 'buildfolder' "http://${lanIp}:8123$p"
    Check ($r.code -eq '404' -and (NoLeak $r)) "wasm-release root: $p -> 404"
}
$r = Req 'buildfolder-wasm' "http://${lanIp}:8123/TaidaFlowApp.wasm" @('-H', 'Accept-Encoding: gzip')
Check ($r.code -eq '200' -and (Header $r.headers 'content-encoding') -eq '' -and (Sha $r.body) -eq (Sha (Join-Path $devWeb 'TaidaFlowApp.wasm'))) 'wasm-release root: TaidaFlowApp.wasm 200 (no .gz there -> identity)'
$rc = NginxScript 'nginx-web.ps1' @('-Action', 'reload', '-WebRoot', $webDeployed) (Join-Path $out 'nginx-reload-back.txt')
Check ($rc -eq 0) 'reload back to <exe folder>\web -> exit 0'
Start-Sleep -Seconds 1
$npids = NginxPids

# ---------------------------------------------------------------- /exports served by nginx
$fx = $small[0]
$fxSha = Sha (Join-Path $exportsDir $fx)
foreach ($h in $hosts) {
    $base = "http://${h}:8123"
    Note "--- /exports, host $h"
    $r = Req "export-fixture-$h" "$base/exports/$fx"
    Check ($r.code -eq '200' -and (Sha $r.body) -eq $fxSha) "$h /exports/$fx 200, body = file"
    Check ((Header $r.headers 'content-type') -eq 'text/csv; charset=utf-8' -and (Header $r.headers 'content-disposition') -eq "attachment; filename=`"$fx`"" -and (Header $r.headers 'cache-control') -eq 'no-store' -and (Header $r.headers 'access-control-allow-origin') -eq '*') "$h /exports headers: text/csv; charset=utf-8, attachment, no-store, ACAO *"
    Check ((Header $r.headers 'accept-ranges') -eq 'bytes' -and (Header $r.headers 'etag') -ne '' -and (Header $r.headers 'last-modified') -ne '' -and (Header $r.headers 'content-encoding') -eq '') "$h /exports Accept-Ranges: bytes, ETag, Last-Modified, no Content-Encoding"
    $rz = Req "export-gzip-$h" "$base/exports/$fx" @('-H', 'Accept-Encoding: gzip')
    Check ($rz.code -eq '200' -and (Header $rz.headers 'content-encoding') -eq '' -and (Sha $rz.body) -eq $fxSha) "$h /exports with Accept-Encoding: gzip -> identity (no gzip on /exports)"
    $bad = @('/exports/', '/exports', '/exports/foo.csv', '/exports/w2050fx01_20260102_000001.txt', '/exports/w2050fx01_20260102_000001.csv.gz',
             '/exports/../w2050-secret.txt', '/exports/..%2Fw2050-secret.txt', '/exports/%2e%2e%5Cw2050secret_20260101_000000.csv',
             '/exports/..%5Cw2050secret_20260101_000000.csv', '/exports/sub/w2050fx01_20260102_000001.csv',
             '/exports/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_20260102_000001.csv', '/exports/w2050fx01_2026010_000001.csv',
             '/exports/w%2050_20260102_000001.csv', '/exports/nothere_20260102_000001.csv', '/exports/W2050FX01_20260102_000001.CSV')
    foreach ($a in $bad) {
        $ra = Req "export-bad-$h" "$base$a"
        Check (($ra.code -in '301', '400', '403', '404') -and $ra.code -ne '200' -and (NoLeak $ra)) "$h $a -> $($ra.code) (not served)"
    }
    $rp = Req "export-post-$h" "$base/exports/$fx" @('-X', 'POST')
    Check ($rp.code -eq '405') "$h POST /exports/<file> -> 405"
}
Note '--- junction in the export folder (created after nginx started)'
$null = New-Item -ItemType Junction -Path (Join-Path $exportsDir 'w2050junction_20260101_000000.csv') -Target (Join-Path $jt 'outside')
$r = Req 'export-junction-nginx' "http://${lanIp}:8123/exports/w2050junction_20260101_000000.csv"
Check ($r.code -eq '404' -and (NoLeak $r)) "junction named like an export file, via nginx 8123 -> $($r.code) (directory never served)"
$r = Req 'export-junction-nginx-slash' "http://${lanIp}:8123/exports/w2050junction_20260101_000000.csv/w2050junction_20260101_000000.csv"
Check ($r.code -eq '404' -and (NoLeak $r)) "file below the junction via nginx -> $($r.code)"
$r = Req 'export-junction-app' "http://${lanIp}:8124/exports/w2050junction_20260101_000000.csv"
Check (($r.code -in '403', '404') -and (NoLeak $r)) "same junction via the app's 8124 fallback -> $($r.code)"
$rc = NginxScript 'nginx-web.ps1' @('-Action', 'reload') (Join-Path $out 'nginx-reload-junction-exports.txt')
Check ($rc -eq 5) 'nginx-web.ps1 -Action reload with a junction in the export folder -> exit 5 (refused, running config kept)'
RemoveJunction (Join-Path $exportsDir 'w2050junction_20260101_000000.csv')

# ---------------------------------------------------------------- D7 (e): export requested like the page, downloaded from nginx
Note '=== D7 (e): history export over the Proxy Mirror (like the web page), download from nginx'
$e1 = MirrorExport 'nginx' $lanIp 'w2050nginx'
Check ($e1.rc -eq 0 -and $e1.state -eq 'done' -and $e1.port -eq '8123' -and $e1.url -eq "/exports/$($e1.fileName)") "historyExportStatus: state done, downloadPort = 8123, url = /exports/$($e1.fileName)"
$e1Path = Join-Path $exportsDir $e1.fileName
$logBefore = @(AppLog 'nginx').Count
$r = Req 'd7e-download-lan-8123' "http://${lanIp}:8123/exports/$($e1.fileName)"
$e1Sha = if (Test-Path $e1Path) { Sha $e1Path } else { '' }
Check ($r.code -eq '200' -and $e1Sha -ne '' -and (Sha $r.body) -eq $e1Sha) "curl http://${lanIp}:8123/exports/$($e1.fileName) -> 200, sha256 = file in the export folder ($e1Sha)"
Check ((Header $r.headers 'server') -eq 'nginx' -and (Header $r.headers 'content-disposition') -eq "attachment; filename=`"$($e1.fileName)`"") 'the response comes from nginx (Server: nginx, attachment)'
Start-Sleep -Seconds 2
$appLines = @(AppLog 'nginx')
$hits = @($appLines | Where-Object { $_ -match [regex]::Escape($e1.fileName) -and $_ -match '\[AppHttpServer\]|\[ExportHTTP\]' })
Note "  app log lines with $($e1.fileName) and [AppHttpServer]/[ExportHTTP]: $($hits.Count)"
$appLines | Where-Object { $_ -match [regex]::Escape($e1.fileName) } | ForEach-Object { Note "    $_" }
Check ($hits.Count -eq 0) 'app log has NO [AppHttpServer]/[ExportHTTP] entry for the nginx download (it did not go through Qt)'
$r = Req 'd7e-download-8124-fallback' "http://127.0.0.1:8124/exports/$($e1.fileName)"
Check ($r.code -eq '200' -and (Sha $r.body) -eq $e1Sha) '8124 fallback (AppHttpServer /exports) still serves the same file'
Start-Sleep -Seconds 2
$hits2 = @(AppLog 'nginx' | Where-Object { $_ -match [regex]::Escape("[AppHttpServer] GET /exports/$($e1.fileName)") })
$hits2 | ForEach-Object { Note "    $_" }
Check ($hits2.Count -ge 1) 'positive control: the 8124 request IS logged by AppHttpServer'
$r = Req 'd7e-range-8124' "http://127.0.0.1:8124/exports/$($e1.fileName)" @('-r', '0-9')
Note ("  8124 fallback with Range 0-9 -> {0} ({1} bytes; the fallback does not support Range)" -f $r.code, $r.size)

# ---------------------------------------------------------------- D5 / D8: large file through nginx (LAN IP)
Note "=== D5/D8: large file $bigName ($bigLen bytes) via http://${lanIp}:8123"
$bigUrl = "http://${lanIp}:8123/exports/$bigName"
$full = Join-Path $dl 'full.csv'
$s = SampledCurl 'full-download' @('-s', '-o', $full, '-D', (Join-Path $out 'big-full.headers.txt'), '-w', '%{http_code} %{size_download} %{speed_download}', $bigUrl)
$fullSha = Sha $full
Check ($s.exit -eq 0 -and $s.w -like '200 *' -and (Get-Item $full).Length -eq $bigLen -and $fullSha -eq $bigSha) "full download 200, $bigLen bytes, sha256 = original"
Check ($s.nGrowWs -lt 64MB -and $s.nGrowPm -lt 64MB) ("nginx memory did not grow with the file (WS +{0:N1} MB, private +{1:N1} MB, file {2:N0} MB)" -f ($s.nGrowWs / 1MB), ($s.nGrowPm / 1MB), ($bigLen / 1MB))
Check ($s.aGrowWs -lt 64MB -and $s.aGrowPm -lt 64MB) ("app memory did not grow (WS +{0:N1} MB, private +{1:N1} MB)" -f ($s.aGrowWs / 1MB), ($s.aGrowPm / 1MB))
Remove-Item $full -Force
$bh = Get-Content -Raw (Join-Path $out 'big-full.headers.txt')
$bigEtag = Header $bh 'etag'
Check ((Header $bh 'accept-ranges') -eq 'bytes' -and $bigEtag -ne '' -and (Header $bh 'last-modified') -ne '') "large file: Accept-Ranges: bytes, ETag $bigEtag, Last-Modified $(Header $bh 'last-modified')"
# (a) first MiB
$r = Req 'd8a-range-first-mib' $bigUrl @('-r', '0-1048575')
Check ($r.code -eq '206' -and (Header $r.headers 'content-range') -eq "bytes 0-1048575/$bigLen" -and $r.size -eq 1048576 -and (Sha $r.body) -eq (ShaOfRange $big 0 1048576)) "D8(a) -r 0-1048575 -> 206, Content-Range: bytes 0-1048575/$bigLen, body = first MiB"
$mid = [int64][math]::Floor($bigLen / 2)
$r = Req 'd8a-range-middle' $bigUrl @('-r', "$mid-$($mid + 65535)")
Check ($r.code -eq '206' -and (Header $r.headers 'content-range') -eq "bytes $mid-$($mid + 65535)/$bigLen" -and (Sha $r.body) -eq (ShaOfRange $big $mid 65536)) 'range in the middle of the file -> 206 with the right bytes'
$r = Req 'd8a-range-suffix' $bigUrl @('-r', '-100')
Check ($r.code -eq '206' -and (Header $r.headers 'content-range') -eq "bytes $($bigLen - 100)-$($bigLen - 1)/$bigLen" -and (Sha $r.body) -eq (ShaOfRange $big ($bigLen - 100) 100)) 'suffix range -100 -> 206, last 100 bytes'
$r = Req 'd8-range-unsatisfiable' $bigUrl @('-r', "$bigLen-")
Check ($r.code -eq '416') "range beyond the end -> 416"
# max_ranges 1: several ranges -> the whole file (200), no multipart
$r = Req 'd8-multi-range' $bigUrl @('-r', '0-9,100-109')
Check ($r.code -eq '200' -and $r.size -eq $bigLen -and (Header $r.headers 'content-type') -eq 'text/csv; charset=utf-8') 'max_ranges 1: two ranges -> 200 with the whole file (no multipart/byteranges)'
Remove-Item $r.body -Force
# (c) If-Range
$qBigEtag = $bigEtag -replace '"', '\"'
$r = Req 'd8c-if-range-current' $bigUrl @('-r', '0-1023', '-H', "If-Range: $qBigEtag")
Check ($r.code -eq '206' -and $r.size -eq 1024) 'If-Range with the current ETag -> 206'
$r = Req 'd8c-if-range-stale' $bigUrl @('-r', '0-1023', '-H', 'If-Range: \"5f000000-1234\"')
Check ($r.code -eq '200' -and $r.size -eq $bigLen -and (Sha $r.body) -eq $bigSha) 'D8(c) If-Range with an old ETag -> 200 with the whole file (sha256 = original)'
Remove-Item $r.body -Force
# (b) + (d) interrupted download, then resume with curl -C -
$part = Join-Path $dl 'resume.csv'
$s1 = SampledCurl 'd8b-interrupted' @('-s', '--limit-rate', '150M', '--max-time', '3', '-o', $part, '-w', '%{http_code} %{size_download}', $bigUrl)
$partLen = if (Test-Path $part) { (Get-Item $part).Length } else { 0 }
Check ($s1.exit -eq 28 -and $partLen -gt 0 -and $partLen -lt $bigLen) "D8(b) download interrupted (curl exit $($s1.exit) = timeout) after $partLen of $bigLen bytes"
$s2 = SampledCurl 'd8b-resume' @('-s', '-C', '-', '-o', $part, '-D', (Join-Path $out 'resume.headers.txt'), '-w', '%{http_code} %{size_download}', $bigUrl)
$rh = Get-Content -Raw (Join-Path $out 'resume.headers.txt')
Note ("  resume response: {0}; Content-Range: {1}" -f (($rh -split "`r?`n")[0]), (Header $rh 'content-range'))
$resSha = Sha $part
Check ($s2.exit -eq 0 -and $s2.w -like '206 *' -and (Header $rh 'content-range') -eq "bytes $partLen-$($bigLen - 1)/$bigLen") "resume with curl -C - -> 206, Content-Range: bytes $partLen-$($bigLen - 1)/$bigLen"
Check ((Get-Item $part).Length -eq $bigLen -and $resSha -eq $bigSha) "resumed file $bigLen bytes, sha256 = original ($resSha)"
Check ($s2.nGrowWs -lt 64MB -and $s2.nGrowPm -lt 64MB -and $s1.nGrowWs -lt 64MB) ("D8(d) nginx memory flat while interrupted + resumed (WS +{0:N1} / +{1:N1} MB, private +{2:N1} MB)" -f ($s1.nGrowWs / 1MB), ($s2.nGrowWs / 1MB), ($s2.nGrowPm / 1MB))
Remove-Item $part -Force

# ---------------------------------------------------------------- D7 (d): cleanup while nginx sends the oldest file
Note '=== D7 (d): export cleanup (20 files) while nginx is sending the oldest file'
Note ("  export folder before: {0} csv file(s)" -f @(Get-ChildItem $exportsDir -Filter *.csv).Count)
$slow = Join-Path $dl 'slow.csv'
$argLine = "-s --limit-rate 40M -o `"$slow`" -w `"%{http_code} %{size_download}`" $bigUrl"
$slowOut = Join-Path $out 'd7d-slow-curl-w.txt'
$cp = Start-Process curl.exe -ArgumentList $argLine -PassThru -WindowStyle Hidden -RedirectStandardOutput $slowOut
$null = $cp.Handle
Start-Sleep -Seconds 3
Note ("  slow download running (curl pid {0}), {1} bytes so far" -f $cp.Id, $(if (Test-Path $slow) { (Get-Item $slow).Length } else { 0 }))
$e2 = MirrorExport 'cleanup' $lanIp 'w2050clean'
Check ($e2.rc -eq 0 -and $e2.state -eq 'done' -and $e2.port -eq '8123') 'second export done (21 files -> cleanup of the oldest)'
Start-Sleep -Seconds 1
$cl = @(AppLog 'nginx' | Where-Object { $_ -match 'cleanup:' })
$cl | Select-Object -Last 2 | ForEach-Object { Note "    $_" }
$bigExists = Test-Path $big
Note ("  after the cleanup: $bigName exists = {0}; export folder {1} csv file(s); download still running = {2}" -f $bigExists, @(Get-ChildItem $exportsDir -Filter *.csv).Count, (-not $cp.HasExited))
$r = Req 'd7d-new-request-after-cleanup' $bigUrl @('-r', '0-9')
Note "  a NEW request for $bigName after the cleanup -> $($r.code)"
$null = $cp.WaitForExit(120000)
$sw = (Get-Content -Raw $slowOut).Trim()
$slowSha = if (Test-Path $slow) { Sha $slow } else { '' }
Note ("  running download finished: curl exit {0}, -w '{1}', {2} bytes, sha256 {3} (original {4})" -f $cp.ExitCode, $sw, $(if (Test-Path $slow) { (Get-Item $slow).Length } else { 0 }), $slowSha, $bigSha)
$lastCl = if ($cl.Count) { $cl[-1] } else { '' }
$removed = $lastCl -match ('removed 1 \[' + [regex]::Escape($bigName) + '\]')
$failedDel = $lastCl -match ('failed 1 \[' + [regex]::Escape($bigName) + '\]')
Note ("  RESULT (Windows, nginx {0}): the cleanup's QFile::remove of the file being sent {1}; the transfer in progress {2}; a new request gets {3}" -f `
      '1.30.5', $(if ($removed) { 'SUCCEEDED (nginx opens files with FILE_SHARE_DELETE)' } elseif ($failedDel) { 'FAILED (logged, kept for the next cleanup)' } else { 'did not run' }),
      $(if ($cp.ExitCode -eq 0 -and $slowSha -eq $bigSha) { 'completed with the full, correct file' } else { "ended with curl exit $($cp.ExitCode)" }), $r.code)
Check (($removed -or $failedDel) -and ($removed -eq (-not $bigExists))) 'cleanup outcome recorded and consistent with the folder (removed -> gone; failed -> logged and kept)'
Check ($cp.ExitCode -eq 0 -and $slowSha -eq $bigSha) 'the download that was running during the cleanup completed with the correct file'
if (Test-Path $slow) { Remove-Item $slow -Force }

# ---------------------------------------------------------------- mirror upgrade still 101
foreach ($h in $hosts) {
    $wsOut = Join-Path $out ("ws-$h.txt")
    & curl.exe -s -i -N --max-time 3 -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' `
        -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' -H "Origin: http://${h}:8123" -o $wsOut "http://${h}:8125/mirror"
    $wsFirst = (Get-Content $wsOut -TotalCount 1)
    Note "  ws://${h}:8125/mirror upgrade -> $wsFirst (curl exit $LASTEXITCODE; 28 = kept open until --max-time)"
    Check ($wsFirst -match '^HTTP/1.1 101') "$h mirror WebSocket upgrade 8125 -> 101"
}

# ---------------------------------------------------------------- stop nginx, close app
Note '=== nginx-stop.ps1'
$npids = NginxPids
Copy-Item (Join-Path $root 'build\nginx\conf\taidaflow.conf') (Join-Path $out 'rendered-taidaflow.conf')
$rc = NginxScript 'nginx-stop.ps1' @() (Join-Path $out 'nginx-stop.txt')
Start-Sleep -Milliseconds 500
$leftN = @($npids | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue } | Where-Object { $_ })
Check ($rc -eq 0 -and @(Get-NetTCPConnection -State Listen -LocalPort 8123 -ErrorAction SilentlyContinue).Count -eq 0 -and $leftN.Count -eq 0) "nginx-stop.ps1 exit 0: no listener on 8123, our nginx pids ($($npids -join ',')) gone"
$allN = @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { $preNginx -notcontains $_.Id })
Check ($allN.Count -eq 0) 'no nginx.exe left that this run started'
Copy-Item (Join-Path $root 'build\nginx\logs\error.log') (Join-Path $out 'nginx-error.log') -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root 'build\nginx\logs\access.log') (Join-Path $out 'nginx-access.log') -ErrorAction SilentlyContinue
CloseApp
Start-Sleep -Seconds 1
Remove-Item Env:\TAIDAFLOW_DOWNLOAD_PORT -ErrorAction SilentlyContinue

# ---------------------------------------------------------------- scenario default: no TAIDAFLOW_DOWNLOAD_PORT -> 8124
Note '=== scenario default: TAIDAFLOW_DOWNLOAD_PORT not set'
if (-not (Launch 'default')) { Note 'app not started'; $fail.Add('default launch') } else {
    $wl = (AppLog 'default') -join "`n"
    Check ($wl -match [regex]::Escape('[Export] TAIDAFLOW_DOWNLOAD_PORT is not set - download links use port 8124 (AppHttpServer)')) 'app log: not set -> port 8124'
    $e3 = MirrorExport 'default' '127.0.0.1' 'w2050default'
    Check ($e3.rc -eq 0 -and $e3.state -eq 'done' -and $e3.port -eq '8124') 'historyExportStatus downloadPort = 8124 without the variable'
    $r = Req 'default-download-8124' "http://127.0.0.1:8124/exports/$($e3.fileName)"
    Check ($r.code -eq '200' -and (Sha $r.body) -eq (Sha (Join-Path $exportsDir $e3.fileName))) 'download from the app on 8124 as before'
}
CloseApp
Start-Sleep -Seconds 1
$env:TAIDAFLOW_DOWNLOAD_PORT = 'abc'
Note '=== scenario invalid: TAIDAFLOW_DOWNLOAD_PORT=abc'
if (-not (Launch 'invalid')) { Note 'app not started'; $fail.Add('invalid launch') } else {
    $wl = (AppLog 'invalid') -join "`n"
    Check ($wl -match [regex]::Escape('[Export] TAIDAFLOW_DOWNLOAD_PORT="abc" is not a port (1..65535) - ignored, download links use port 8124 (AppHttpServer)')) 'app log: invalid value -> warning, port 8124'
    $e4 = MirrorExport 'invalid' '127.0.0.1' 'w2050invalid'
    Check ($e4.rc -eq 0 -and $e4.port -eq '8124') 'historyExportStatus downloadPort = 8124 with an invalid value'
}
CloseApp
Remove-Item Env:\TAIDAFLOW_DOWNLOAD_PORT -ErrorAction SilentlyContinue
CloseSim
Start-Sleep -Seconds 1
Check ((Listeners).Count -eq 0) 'end: no listener on 502/8123/8124/8125/18125'

# ---------------------------------------------------------------- fixtures away
foreach ($n in @($bigName) + $small + @($e1.fileName, $e2.fileName, $e3.fileName, $e4.fileName)) {
    if ($n) { $pth = Join-Path $exportsDir $n; if (Test-Path -LiteralPath $pth) { Remove-Item -LiteralPath $pth -Force } }
}
Remove-Item -LiteralPath (Join-Path $root 'build\desktop\w2050-secret.js'), (Join-Path $cwd 'w2050-secret.txt'), (Join-Path $cwd 'w2050secret_20260101_000000.csv'),
    (Join-Path $webDeployed '.w2050hidden.js'), (Join-Path $webDeployed 'w2050-notweb.txt') -Force -ErrorAction SilentlyContinue
if (@(Get-ChildItem $jt -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue).Count -eq 0) { Remove-Item -LiteralPath $jt -Recurse -Force }
Note ("fixtures removed; export folder now: {0} csv file(s)" -f @(Get-ChildItem $exportsDir -Filter *.csv).Count)
foreach ($f in @(Get-ChildItem $out -Filter '*.body' | Where-Object { $_.Length -gt 65536 })) {
    Note ("removed large body {0} ({1} bytes, sha256 {2})" -f $f.Name, $f.Length, (Sha $f.FullName))
    Remove-Item -LiteralPath $f.FullName -Force
}
Note ("RESULT: {0} failed check(s)" -f $fail.Count)
$fail | ForEach-Object { Note "  failed: $_" }
if ($fail.Count) { exit 1 } else { exit 0 }

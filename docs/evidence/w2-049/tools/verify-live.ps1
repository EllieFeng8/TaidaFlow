# w2-049 live check of the desktop HTTP service (AppHttpServer singleton on 0.0.0.0:8124).
# No UI test: no browser, no screenshots. Every request is made with curl.exe; results are judged
# by status codes / headers / hashes and written to docs\evidence\w2-049\live\.
#
# Safety: the app is started ONLY through scripts\run-desktop.ps1 -DeviceProfile simulator
# (scripts\safety_probe.ps1 must say SAFE; working directory build\runtime-cwd; probe record in
# docs\evidence\w2-049\safety-probe.log). The simulator is started through scripts\run-simulator.ps1
# (refuses when 502 is taken). Only processes started here are closed (WM_CLOSE first). No network,
# firewall or system setting is changed. While 502/8124/8125/18125 have a listener the script waits
# (re-check every 30 s, at most 30 min) and never stops the owner.
#
# Scenarios (one simulator, three app launches):
#   web    : <exe folder>\web deployed by scripts\deploy-web.ps1 (with .gz), TAIDAFLOW_WEB_DIR unset.
#            Page / .wasm / .js / gzip / ETag-304 / HEAD / redirect / traversal / downloads from
#            127.0.0.1 and the LAN IPv4; mirror WebSocket upgrade on 8125 -> 101.
#   env    : TAIDAFLOW_WEB_DIR=build\wasm-release (takes precedence over <exe folder>\web).
#   noweb  : TAIDAFLOW_WEB_DIR=<missing folder>, <exe folder>\web removed and the development
#            default's TaidaFlowApp.html renamed for the run (restored afterwards) -> warning only,
#            /exports downloads still work.
# Test fixture (dev-only, stated in the report): one CSV with a valid export name is written into
# build\runtime-cwd\exports (the app has no plant data here, and exports are started from the UI),
# plus a secret file next to the exports folder for the traversal attempts.
#
# Usage: powershell -ExecutionPolicy Bypass -File docs\evidence\w2-049\tools\verify-live.ps1
# Exit 0 = all checks passed; 1 = a check failed; 2 = not built / not deployed; 3 = ports busy for
# 30 min, simulator not started or safety probe not SAFE (app not started).
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$ev    = Split-Path -Parent $tools                        # docs\evidence\w2-049
$root  = (Resolve-Path (Join-Path $ev '..\..\..')).Path   # taidaflow
$exe   = Join-Path $root 'build\desktop\TaidaFlowApp.exe'
$webDeployed = Join-Path $root 'build\desktop\web'
$devWeb = Join-Path $root 'build\wasm-release'
$out   = Join-Path $ev 'live'
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -File | Remove-Item -Force
$fail = New-Object System.Collections.Generic.List[string]
$app = $null
$sim = $null
$renamed = $null

function Note($m) { Write-Output $m; Add-Content -Path (Join-Path $out 'summary.txt') -Value $m -Encoding utf8 }
function Check($ok, $what) {
    if ($ok) { Note "  PASS $what" } else { Note "  FAIL $what"; $fail.Add($what) }
}
function Listeners { @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 502, 8124, 8125, 18125 }) }
function Sha($path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash }
function Header($text, $name) {
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match ('^' + [regex]::Escape($name) + ':\s*(.*)$')) { return $Matches[1].Trim() }
    }
    return ''
}
# One curl request. Returns code, headers (last response), body file, sizes, time.
$script:reqNo = 0
function Req($label, $url, [string[]]$extra = @()) {
    $script:reqNo++
    $tag = '{0:D2}-{1}' -f $script:reqNo, ($label -replace '[^A-Za-z0-9_.-]', '_')
    $hdr = Join-Path $out "$tag.headers.txt"
    $body = Join-Path $out "$tag.body"
    $w = & curl.exe -s --path-as-is --max-time 60 -D $hdr -o $body -w '%{http_code} %{size_download} %{time_total}' @extra $url
    $rc = $LASTEXITCODE
    $parts = "$w" -split ' '
    $h = if (Test-Path $hdr) { Get-Content -Raw $hdr } else { '' }
    $r = [pscustomobject]@{ label = $label; url = $url; code = $parts[0]; size = [int64]$parts[1]; time = $parts[2];
                            headers = $h; body = $body; curl = $rc }
    Note ("  {0} {1} -> {2} ({3} bytes, {4} s) {5}" -f $label, $url, $r.code, $r.size, $r.time, (($extra -join ' ') -replace '\s+', ' '))
    return $r
}
function SmallBody($r) { if ((Test-Path $r.body) -and (Get-Item $r.body).Length -lt 4096) { return (Get-Content -Raw $r.body) } else { return '' } }
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
trap {
    Note "ERROR: $($_.Exception.Message)"
    CloseApp; CloseSim
    if ($script:renamed) { Rename-Item -LiteralPath $script:renamed -NewName 'TaidaFlowApp.html'; Note 'restored TaidaFlowApp.html' }
    Remove-Item Env:\TAIDAFLOW_WEB_DIR -ErrorAction SilentlyContinue
    exit 1
}

if (-not (Test-Path $exe)) { Note "not built: $exe"; exit 2 }
if (-not (Test-Path (Join-Path $webDeployed 'TaidaFlowApp.wasm.gz'))) { Note "not deployed: run scripts\deploy-web.ps1 first"; exit 2 }
if (Get-Process TaidaFlowApp -ErrorAction SilentlyContinue) { Note 'TaidaFlowApp already running (not ours) - not touching it'; exit 3 }

# Wait while the ports are taken (every 30 s, max 30 min).
$deadline = (Get-Date).AddMinutes(30)
while ((Listeners).Count -gt 0) {
    $l = Listeners | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid=$($_.OwningProcess)" }
    Note ("ports busy: " + ($l -join ', ') + " - re-check in 30 s (owner not stopped)")
    if ((Get-Date) -gt $deadline) { Note 'ports still busy after 30 min'; exit 3 }
    Start-Sleep -Seconds 30
}

$lan = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
         Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
         Sort-Object InterfaceMetric | ForEach-Object { $_.IPAddress })
Note ("local IPv4: " + ($lan -join ', '))
$lanIp = if ($lan.Count) { $lan[0] } else { '' }
Note "LAN IPv4 used for the LAN requests: $lanIp"

# Fixture: a CSV with a valid export name + a secret file outside the exports folder.
$cwd = Join-Path $root 'build\runtime-cwd'
$exportsDir = Join-Path $cwd 'exports'
New-Item -ItemType Directory -Force $exportsDir | Out-Null
$csvName = 'web-w2049_20260926_120000.csv'
$csvPath = Join-Path $exportsDir $csvName
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append([char]0xFEFF).Append('"seq","time"' + "`r`n")
for ($i = 1; $i -le 20000; $i++) { [void]$sb.Append(('"{0}","2026/09/26 12:00:{1:D2}"' -f $i, ($i % 60)) + "`r`n") }
[System.IO.File]::WriteAllText($csvPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
$csvSha = Sha $csvPath
Set-Content -Path (Join-Path $cwd 'w2049-secret.txt') -Value 'SECRET-OUTSIDE-EXPORTS' -Encoding ascii
Set-Content -Path (Join-Path $cwd 'secret_20260926_120000.csv') -Value 'SECRET-OUTSIDE-EXPORTS-CSV' -Encoding ascii
Note "fixture: $csvPath ($((Get-Item $csvPath).Length) bytes, sha256 $csvSha)"

# Runs a launcher script with its stdout in a file (never a pipe: the started program could hold
# a pipe open, see README), waits for the launcher only, returns its exit code.
function RunLauncher($script, [string[]]$scriptArgs, $outFile) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"") + $scriptArgs
    $runner = Start-Process powershell -PassThru -WindowStyle Hidden -RedirectStandardOutput $outFile `
                  -RedirectStandardError "$outFile.stderr" -ArgumentList $argList
    $null = $runner.Handle
    $runner.WaitForExit()
    return $runner.ExitCode
}

# Simulator first (simulator profile needs its five endpoints before the app starts).
$simTxt = Join-Path $out 'run-simulator.txt'
$rc = RunLauncher (Join-Path $root 'scripts\run-simulator.ps1') @('-LogFile', "`"$(Join-Path $out 'simulator.log')`"") $simTxt
Note ("run-simulator: " + ((Get-Content $simTxt) -join ' ') + " (exit $rc)")
$m = Select-String -CaseSensitive -Path $simTxt -Pattern '^SIM_PID=(\d+)' | Select-Object -First 1
if ($m) { $sim = Get-Process -Id ([int]$m.Matches[0].Groups[1].Value); $null = $sim.Handle }
if ($rc -ne 0 -or -not $sim) { Note 'simulator not started'; CloseSim; exit 3 }

function Launch($scenario) {
    $log = Join-Path $out "app-$scenario.log"
    $runTxt = Join-Path $out "run-desktop-$scenario.txt"
    $rc = RunLauncher (Join-Path $root 'scripts\run-desktop.ps1') @('-DeviceProfile', 'simulator', '-Label', "`"w2-049 live $scenario`"",
            '-ProbeLog', "`"$(Join-Path $ev 'safety-probe.log')`"", '-LogFile', "`"$log`"") $runTxt
    # case-sensitive and anchored: the probe output also has 'pid=<simulator pid>' lines
    $m = Select-String -CaseSensitive -Path $runTxt -Pattern '^PID=(\d+)' | Select-Object -First 1
    if ($m) { $script:app = Get-Process -Id ([int]$m.Matches[0].Groups[1].Value); $null = $script:app.Handle }
    Note ("run-desktop ($scenario): " + ((Get-Content $runTxt | Select-Object -Last 3) -join ' | ') + " (exit $rc)")
    if ($rc -ne 0 -or -not $script:app) { return $false }
    $t0 = Get-Date
    do {
        Start-Sleep -Milliseconds 500
        $up = @(Get-NetTCPConnection -State Listen -LocalPort 8124 -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $script:app.Id })
    } while ($up.Count -eq 0 -and ((Get-Date) - $t0).TotalSeconds -lt 30 -and -not $script:app.HasExited)
    Start-Sleep -Seconds 3
    return $up.Count -gt 0
}
function WebLines($scenario) {
    $log = Join-Path $out "app-$scenario.log"
    $lines = @(Get-Content $log -Encoding utf8 | Where-Object { $_ -match '\[Web\]|\[AppHttpServer\] (listening|NOT|static|downloads|stopped)|\[Export\] download mount' })
    Note "  log lines ($scenario):"
    $lines | ForEach-Object { Note "    $_" }
    return ($lines -join "`n")
}

$failAtStart = 0
# ---------------------------------------------------------------- scenario: web
Remove-Item Env:\TAIDAFLOW_WEB_DIR -ErrorAction SilentlyContinue
Note "=== scenario web (TAIDAFLOW_WEB_DIR unset, <exe folder>\web deployed)"
if (-not (Launch 'web')) { Note 'app not started / 8124 not listening'; CloseApp; CloseSim; exit 3 }
$l8124 = @(Get-NetTCPConnection -State Listen -LocalPort 8124 -ErrorAction SilentlyContinue)
Check ($l8124.Count -eq 1 -and $l8124[0].LocalAddress -eq '0.0.0.0' -and $l8124[0].OwningProcess -eq $app.Id) "0.0.0.0:8124 listening, owned by the app pid $($app.Id)"
$wl = WebLines 'web'
Check ($wl -match [regex]::Escape('[Web] TAIDAFLOW_WEB_DIR is not set')) 'log: TAIDAFLOW_WEB_DIR is not set'
Check ($wl -match '\[Web\] web page folder \(<exe folder>/web\): .*build\\desktop\\web') 'log: web page folder = <exe folder>/web'
Check ($wl -match [regex]::Escape('[Web] HTTP service listening on 0.0.0.0:8124 (web page served, /exports downloads)')) 'log: HTTP service listening on 0.0.0.0:8124'
Check ($wl -match [regex]::Escape('[AppHttpServer] listening on 0.0.0.0:8124 (thread AppHttpServerThread); mounts: /exports, /')) 'log: AppHttpServer mounts /exports and /'

$wasm = Join-Path $webDeployed 'TaidaFlowApp.wasm'
$wasmLen = (Get-Item $wasm).Length
$gzLen = (Get-Item "$wasm.gz").Length
$wasmSha = Sha $wasm
$gzSha = Sha "$wasm.gz"
Note "deployed TaidaFlowApp.wasm $wasmLen bytes, .gz $gzLen bytes"
$hosts = @('127.0.0.1')
if ($lanIp) { $hosts += $lanIp }
foreach ($h in $hosts) {
    $base = "http://${h}:8124"
    Note "--- host $h"
    $r = Req "html-$h" "$base/TaidaFlowApp.html"
    Check ($r.code -eq '200') "$h html 200"
    Check ((Header $r.headers 'content-type') -eq 'text/html; charset=utf-8') "$h html Content-Type text/html"
    Check ((Header $r.headers 'cache-control') -eq 'no-cache') "$h html Cache-Control no-cache"
    Check ((Header $r.headers 'cross-origin-opener-policy') -eq 'same-origin' -and (Header $r.headers 'cross-origin-embedder-policy') -eq 'require-corp' -and (Header $r.headers 'cross-origin-resource-policy') -eq 'same-origin') "$h html COOP/COEP/CORP"
    Check ((Sha $r.body) -eq (Sha (Join-Path $webDeployed 'TaidaFlowApp.html'))) "$h html body = deployed file"
    $rg = Req "html-gzip-$h" "$base/TaidaFlowApp.html" @('-H', 'Accept-Encoding: gzip')
    Check ($rg.code -eq '200' -and (Header $rg.headers 'content-encoding') -eq 'gzip' -and (Header $rg.headers 'content-type') -eq 'text/html; charset=utf-8') "$h html gzip variant"

    $rw = Req "wasm-identity-$h" "$base/TaidaFlowApp.wasm"
    Check ($rw.code -eq '200' -and (Header $rw.headers 'content-type') -eq 'application/wasm') "$h wasm 200 application/wasm"
    Check ([int64](Header $rw.headers 'content-length') -eq $wasmLen -and $rw.size -eq $wasmLen -and (Sha $rw.body) -eq $wasmSha) "$h wasm identity $wasmLen bytes, sha256 equal"
    Check ((Header $rw.headers 'content-encoding') -eq '' -and (Header $rw.headers 'vary') -eq 'Accept-Encoding') "$h wasm identity: no Content-Encoding, Vary"
    $etag = Header $rw.headers 'etag'
    $rz = Req "wasm-gzip-$h" "$base/TaidaFlowApp.wasm" @('-H', 'Accept-Encoding: gzip, deflate, br')
    Check ($rz.code -eq '200' -and (Header $rz.headers 'content-encoding') -eq 'gzip' -and (Header $rz.headers 'content-type') -eq 'application/wasm') "$h wasm gzip: Content-Encoding gzip, MIME application/wasm"
    Check ($rz.size -eq $gzLen -and (Sha $rz.body) -eq $gzSha) "$h wasm gzip $gzLen bytes = deployed .gz"
    Check ((Gunzip $rz.body) -eq $wasmSha) "$h wasm gzip body decompresses to the .wasm (sha256)"
    $etagGz = Header $rz.headers 'etag'
    # Windows PowerShell 5.1 drops embedded double quotes from native arguments, so the quotes
    # of the entity-tag are escaped for curl.exe (the server must receive "..." as browsers send it).
    $qEtag = $etag -replace '"', '\"'
    $qEtagGz = $etagGz -replace '"', '\"'
    $r304 = Req "wasm-304-$h" "$base/TaidaFlowApp.wasm" @('-H', "If-None-Match: $qEtag")
    Check ($r304.code -eq '304' -and $r304.size -eq 0) "$h wasm second request with ETag -> 304, no body"
    $r304g = Req "wasm-gzip-304-$h" "$base/TaidaFlowApp.wasm" @('-H', 'Accept-Encoding: gzip', '-H', "If-None-Match: $qEtagGz")
    Check ($r304g.code -eq '304' -and $r304g.size -eq 0) "$h wasm gzip second request with its ETag -> 304"
    $rims = Req "html-ims-$h" "$base/TaidaFlowApp.html" @('-H', ('If-Modified-Since: ' + (Header $r.headers 'last-modified')))
    Check ($rims.code -eq '304') "$h html If-Modified-Since -> 304"
    $rhead = Req "wasm-head-$h" "$base/TaidaFlowApp.wasm" @('-I')
    Check ($rhead.code -eq '200' -and [int64](Header $rhead.headers 'content-length') -eq $wasmLen -and $rhead.size -eq 0) "$h HEAD wasm 200, Content-Length $wasmLen, no body"
    foreach ($f in @(@('TaidaFlowApp.js', 'text/javascript; charset=utf-8'), @('qtloader.js', 'text/javascript; charset=utf-8'), @('qtlogo.svg', 'image/svg+xml'))) {
        $rf = Req "$($f[0])-$h" "$base/$($f[0])"
        Check ($rf.code -eq '200' -and (Header $rf.headers 'content-type') -eq $f[1] -and (Sha $rf.body) -eq (Sha (Join-Path $webDeployed $f[0]))) "$h $($f[0]) 200 $($f[1])"
    }
    $rr = Req "root-$h" "$base/"
    Check ($rr.code -eq '302' -and (Header $rr.headers 'location') -eq '/TaidaFlowApp.html') "$h / -> 302 /TaidaFlowApp.html"

    $rd = Req "export-$h" "$base/exports/$csvName"
    Check ($rd.code -eq '200' -and (Sha $rd.body) -eq $csvSha) "$h /exports download 200, body = file"
    Check ((Header $rd.headers 'content-disposition') -eq "attachment; filename=`"$csvName`"" -and (Header $rd.headers 'access-control-allow-origin') -eq '*' -and (Header $rd.headers 'cache-control') -eq 'no-store' -and (Header $rd.headers 'content-type') -eq 'text/csv; charset=utf-8') "$h /exports headers (attachment, ACAO *, no-store, text/csv)"

    $attacks = @('/../w2049-secret.txt', '/%2e%2e/w2049-secret.txt', '/%2E%2E%2Fw2049-secret.txt', '/..%5Cw2049-secret.txt',
                 '/..\w2049-secret.txt', '/exports/../w2049-secret.txt', '/exports/..%2Fsecret_20260926_120000.csv',
                 '/exports/%2e%2e%5Csecret_20260926_120000.csv', '/exports//secret_20260926_120000.csv',
                 '/C:%5CWindows%5Cwin.ini', '/C:/Windows/win.ini', '/%5C%5C127.0.0.1%5Cc$%5CWindows%5Cwin.ini',
                 '/TaidaFlowApp.html::$DATA', '/TaidaFlowApp.html.', '/NUL', '/exports/w2049-secret.txt')
    foreach ($a in $attacks) {
        $ra = Req "attack-$h" "$base$a"
        $b = SmallBody $ra
        Check (($ra.code -in '400', '403', '404') -and -not ($b -match 'SECRET')) "$h attack $a -> $($ra.code)"
    }
    $rp = Req "post-$h" "$base/exports/$csvName" @('-X', 'POST')
    Check ($rp.code -eq '405') "$h POST /exports -> 405"

    # Mirror WebSocket upgrade through the LAN relay (8125) still answers 101.
    $wsOut = Join-Path $out ("ws-$h.txt")
    & curl.exe -s -i -N --max-time 3 -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' `
        -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' -H "Origin: http://${h}:8124" -o $wsOut "http://${h}:8125/mirror"
    $wsFirst = (Get-Content $wsOut -TotalCount 1)
    Note "  ws://${h}:8125/mirror upgrade -> $wsFirst (curl exit $LASTEXITCODE; 28 = kept open until --max-time)"
    Check ($wsFirst -match '^HTTP/1.1 101') "$h mirror WebSocket upgrade 8125 -> 101"
}
CloseApp
Start-Sleep -Seconds 1
$left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 8124, 8125, 18125 })
Check ($left.Count -eq 0) 'after close: no listener on 8124/8125/18125'
$wl2 = Get-Content (Join-Path $out 'app-web.log') -Encoding utf8 | Where-Object { $_ -match '\[AppHttpServer\] stopped|\[Export\] stopped' }
$wl2 | ForEach-Object { Note "    $_" }
Check ([bool]($wl2 -match '\[AppHttpServer\] stopped \(was port 8124\)')) 'log: AppHttpServer stopped on quit'

# ---------------------------------------------------------------- scenario: env
$env:TAIDAFLOW_WEB_DIR = $devWeb
Note "=== scenario env (TAIDAFLOW_WEB_DIR=$devWeb, <exe folder>\web still deployed)"
if (-not (Launch 'env')) { Note 'app not started'; CloseApp; CloseSim; exit 3 }
$wl = WebLines 'env'
Check ($wl -match '\[Web\] web page folder \(TAIDAFLOW_WEB_DIR\): .*build\\wasm-release') 'log: TAIDAFLOW_WEB_DIR wins over <exe folder>/web'
$r = Req 'env-wasm' 'http://127.0.0.1:8124/TaidaFlowApp.wasm' @('-H', 'Accept-Encoding: gzip')
Check ($r.code -eq '200' -and (Header $r.headers 'content-encoding') -eq '' -and (Sha $r.body) -eq (Sha (Join-Path $devWeb 'TaidaFlowApp.wasm'))) 'env: wasm from build\wasm-release (no .gz there -> identity)'
$r = Req 'env-cmakecache' 'http://127.0.0.1:8124/CMakeCache.txt'
Check ($r.code -eq '404') 'env: build file CMakeCache.txt not served (suffix list) -> 404'
$r = Req 'env-ninja' 'http://127.0.0.1:8124/build.ninja'
Check ($r.code -eq '404') 'env: build.ninja -> 404'
CloseApp
Start-Sleep -Seconds 1

# ---------------------------------------------------------------- scenario: noweb
$missing = Join-Path $root 'build\w2-049-no-such-web-dir'
$env:TAIDAFLOW_WEB_DIR = $missing
Remove-Item -LiteralPath $webDeployed -Recurse -Force
Rename-Item -LiteralPath (Join-Path $devWeb 'TaidaFlowApp.html') -NewName 'TaidaFlowApp.html.w2049-hidden'
$renamed = Join-Path $devWeb 'TaidaFlowApp.html.w2049-hidden'
Note "=== scenario noweb (TAIDAFLOW_WEB_DIR=$missing, <exe folder>\web removed, dev default page renamed for this run)"
$ok = Launch 'noweb'
try {
    if (-not $ok) { Note 'app not started'; $fail.Add('noweb launch') } else {
        $wl = WebLines 'noweb'
        Check ($wl -match '\[Web\] candidate TAIDAFLOW_WEB_DIR: .* does not exist, skipped') 'log: env candidate skipped'
        Check ($wl -match '\[Web\] candidate <exe folder>/web: .* does not exist, skipped') 'log: <exe folder>/web skipped'
        Check ($wl -match '\[Web\] candidate development default: .* no TaidaFlowApp.html, skipped') 'log: development default skipped'
        Check ($wl -match '\[Web\] no web page folder found') 'log: warning no web page folder found'
        Check ($wl -match [regex]::Escape('[Web] HTTP service listening on 0.0.0.0:8124 (web page NOT served, /exports downloads)')) 'log: HTTP service still listening'
        $r = Req 'noweb-html' 'http://127.0.0.1:8124/TaidaFlowApp.html'
        Check ($r.code -eq '404') 'noweb: page 404'
        $r = Req 'noweb-export' "http://127.0.0.1:8124/exports/$csvName"
        Check ($r.code -eq '200' -and (Sha $r.body) -eq $csvSha) 'noweb: /exports download still 200'
    }
} finally {
    CloseApp
    Rename-Item -LiteralPath $renamed -NewName 'TaidaFlowApp.html'
    $renamed = $null
    Remove-Item Env:\TAIDAFLOW_WEB_DIR -ErrorAction SilentlyContinue
    Note 'restored build\wasm-release\TaidaFlowApp.html'
}
CloseSim
Start-Sleep -Seconds 1
$left = @(Listeners)
Check ($left.Count -eq 0) 'end: no listener on 502/8124/8125/18125'
Remove-Item -LiteralPath $csvPath, (Join-Path $cwd 'w2049-secret.txt'), (Join-Path $cwd 'secret_20260926_120000.csv') -Force
Note 'fixture files removed; <exe folder>\web was removed by the noweb scenario (re-run scripts\deploy-web.ps1 to deploy again)'
# Keep the evidence folder small: large response bodies were already compared by sha256 above.
foreach ($f in @(Get-ChildItem $out -Filter '*.body' | Where-Object { $_.Length -gt 65536 })) {
    Note ("removed large body {0} ({1} bytes, sha256 {2})" -f $f.Name, $f.Length, (Sha $f.FullName))
    Remove-Item -LiteralPath $f.FullName -Force
}
Note ("RESULT: {0} failed check(s)" -f $fail.Count)
$fail | ForEach-Object { Note "  failed: $_" }
if ($fail.Count) { exit 1 } else { exit 0 }

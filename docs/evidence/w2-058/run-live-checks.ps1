# w2-058 D3 (no browser, no screenshots): deploy the web page, start the desktop app SAFELY and check
# that the served page is the new loading page and nothing else changed.
#   1. scripts\deploy-web.ps1 (build\wasm-release -> build\desktop\web + .gz)            exit 0
#   2. preconditions: no TaidaFlowApp / nginx running, nothing listening on 80/8124/8125/18125
#   3. scripts\run-desktop.ps1 (safety probe first; the app is started only on SAFE; probe log
#      docs\evidence\w2-058\safety-probe.log)                                            exit 0
#   4. 8124 (AppHttpServer): TaidaFlowApp.html body == build\desktop\web\TaidaFlowApp.html ==
#      build\wasm-release\TaidaFlowApp.html (SHA-256), plain and gzip (Content-Encoding: gzip);
#      headers Content-Type "text/html; charset=utf-8", Cache-Control "no-cache", COOP/COEP/CORP;
#      TaidaFlowApp.wasm 200 application/wasm, TaidaFlowApp.js 200, qtloader.js 200
#   5. scripts\nginx-start.ps1 (port 80): the same checks on http://127.0.0.1/ (nginx)
#   6. WebSocket upgrade to ws://127.0.0.1:8125/mirror (the sync endpoint the page uses) -> 101
#   7. scripts\nginx-stop.ps1 (only the nginx started here), app closed with WM_CLOSE
#      (scripts\desktop_input.py close), no listener left on 80/8124/8125/18125/502.
# Everything started here is stopped here (also on failure). Output: docs\evidence\w2-058\30-live\.
# Run it with the output redirected to a FILE (not a pipe: the started app / nginx would inherit it):
#   cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-058\run-live-checks.ps1 > docs\evidence\w2-058\30-live-console.txt 2>&1"
# Exit 0 = all checks passed; 3 = probe not SAFE (app not started); 2 = precondition; 1 = a check failed.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$out = Join-Path $PSScriptRoot '30-live'
New-Item -ItemType Directory -Force $out | Out-Null
$py = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'
$curl = 'curl.exe'
$results = New-Object System.Collections.Generic.List[string]
$fail = $false
function Check([string]$name, [bool]$ok, [string]$detail) {
    $line = "{0} {1}: {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail
    $script:results.Add($line); Write-Output $line
    if (-not $ok) { $script:fail = $true }
}
function Sha([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }
function Header([string[]]$lines, [string]$name) {
    $h = $lines | Where-Object { $_ -match "^${name}:\s*(.*)$" } | Select-Object -Last 1
    if ($h -match "^${name}:\s*(.*)$") { $Matches[1].Trim() } else { '' }
}
# GET url -> body file + header file; returns @{code; headers}
function Get-Url([string]$url, [string]$tag, [switch]$Gzip) {
    $body = Join-Path $out "$tag.body"; $hdr = Join-Path $out "$tag.headers.txt"
    $a = @('-s', '-S', '--max-time', '30', '-D', $hdr, '-o', $body, '-w', '%{http_code}')
    if ($Gzip) { $a += @('-H', 'Accept-Encoding: gzip') }
    $code = & $curl @a $url
    $lines = @(Get-Content -LiteralPath $hdr)
    return @{ code = $code; headers = $lines; body = $body }
}
function Check-Server([string]$base, [string]$tag) {
    $expect = Sha (Join-Path $root 'build\desktop\web\TaidaFlowApp.html')
    $r = Get-Url "$base/TaidaFlowApp.html" "$tag-html"
    Check "$tag html 200" ($r.code -eq '200') "status=$($r.code)"
    $sha = Sha $r.body
    Check "$tag html body = deployed new page" ($sha -eq $expect) "sha256=$sha expected=$expect"
    $text = Get-Content -LiteralPath $r.body -Raw -Encoding utf8
    Check "$tag html is the TaidaFlow page" (($text -match 'taidaflow-wasm-shell') -and ($text -match '<title>TaidaFlow</title>') -and ($text -notmatch 'qtlogo') -and ($text -notmatch 'Loading\.\.\.')) 'marker + title, no qtlogo / Loading...'
    $ct = Header $r.headers 'Content-Type'; $cc = Header $r.headers 'Cache-Control'
    Check "$tag html Content-Type" ($ct -eq 'text/html; charset=utf-8') $ct
    Check "$tag html Cache-Control" ($cc -eq 'no-cache') $cc
    Check "$tag html COOP/COEP/CORP" (((Header $r.headers 'Cross-Origin-Opener-Policy') -eq 'same-origin') -and ((Header $r.headers 'Cross-Origin-Embedder-Policy') -eq 'require-corp') -and ((Header $r.headers 'Cross-Origin-Resource-Policy') -eq 'same-origin')) 'same-origin / require-corp / same-origin'
    Check "$tag html ETag present" ((Header $r.headers 'ETag') -ne '') (Header $r.headers 'ETag')
    $g = Get-Url "$base/TaidaFlowApp.html" "$tag-html-gzip" -Gzip
    $ce = Header $g.headers 'Content-Encoding'
    Check "$tag html gzip" (($g.code -eq '200') -and ($ce -eq 'gzip') -and ((Header $g.headers 'Content-Type') -eq 'text/html; charset=utf-8')) "status=$($g.code) Content-Encoding=$ce"
    $gz = Join-Path $out "$tag-html-gzip.body"
    $unz = & $py -B -c "import gzip,hashlib,sys; print(hashlib.sha256(gzip.open(sys.argv[1]).read()).hexdigest())" $gz
    Check "$tag html gzip body = page" ($unz -eq $expect) "gunzip sha256=$unz"
    foreach ($f in @(@('TaidaFlowApp.wasm', 'application/wasm'), @('TaidaFlowApp.js', ''), @('qtloader.js', ''))) {
        $w = Get-Url "$base/$($f[0])" "$tag-$($f[0])"
        $wct = Header $w.headers 'Content-Type'
        $ok = ($w.code -eq '200') -and (($f[1] -eq '') -or ($wct -eq $f[1]))
        Check "$tag $($f[0])" $ok "status=$($w.code) Content-Type=$wct Cache-Control=$(Header $w.headers 'Cache-Control') bytes=$((Get-Item $w.body).Length)"
        Remove-Item -LiteralPath $w.body -Force
    }
}

# 1. deploy
$dep = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\deploy-web.ps1') 2>&1
$depRc = $LASTEXITCODE
$dep | Out-File -Encoding utf8 (Join-Path $out '01-deploy-web.txt')
Check 'deploy-web' ($depRc -eq 0) "exit=$depRc"
if ($depRc -ne 0) { exit 1 }
$src = Sha (Join-Path $root 'build\wasm-release\TaidaFlowApp.html'); $dst = Sha (Join-Path $root 'build\desktop\web\TaidaFlowApp.html')
Check 'deployed html = build\wasm-release html' ($src -eq $dst) "sha256=$dst"

# 2. preconditions
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 80, 8124, 8125, 18125 })
$procs = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue)
if ($busy.Count -or $procs.Count) {
    Write-Output "precondition failed: listeners=$($busy | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)#$($_.OwningProcess)" }) processes=$($procs | ForEach-Object { "$($_.ProcessName)#$($_.Id)" }) - nothing started"
    exit 2
}

$app = $null; $nginxStarted = $false
try {
    # 3. app (safety probe first)
    $appLog = Join-Path $root 'build\runtime-logs\w2-058-live.log'
    # Not captured into a variable / pipe: the started app would inherit the pipe and the call
    # would only return when the app exits (README). Its output goes to this script's output.
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\run-desktop.ps1') -Label 'w2-058 loading page live check' -LogFile $appLog -ProbeLog (Join-Path $PSScriptRoot 'safety-probe.log')
    $runRc = $LASTEXITCODE
    if ($runRc -ne 0) { Check 'run-desktop (safety probe SAFE + start)' $false "exit=$runRc - app not started"; exit $(if ($runRc -eq 3) { 3 } else { 1 }) }
    Check 'run-desktop (safety probe SAFE + start)' $true "exit=0"
    $app = Get-Process TaidaFlowApp -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $app) { Check 'app process' $false 'not found'; exit 1 }
    $null = $app.Handle
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and -not (Get-NetTCPConnection -LocalPort 8124 -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $app.Id })) { Start-Sleep -Milliseconds 500 }
    Start-Sleep -Seconds 3

    # 4. AppHttpServer 8124
    Check-Server 'http://127.0.0.1:8124' '8124'

    # 6. sync endpoint (WebSocket upgrade through the LAN relay the page connects to)
    $ws = & $py -B (Join-Path $root 'docs\evidence\w2-043\tools\ws_probe.py') connect 127.0.0.1 8125 'http://127.0.0.1:8124' 2 2>&1
    $ws | Out-File -Encoding utf8 (Join-Path $out '10-ws-8125.txt')
    $status = ($ws | Where-Object { $_ -like 'HTTP_STATUS*' }) -join ' '
    Check 'ws://127.0.0.1:8125/mirror upgrade' ($status -match ' 101 ') $status

    # 5. nginx port 80
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\nginx-start.ps1')
    $ngRc = $LASTEXITCODE
    $nginxStarted = ($ngRc -eq 0)
    Check 'nginx-start (port 80)' ($ngRc -eq 0) "exit=$ngRc"
    if ($nginxStarted) { Check-Server 'http://127.0.0.1:80' 'nginx80' }
}
finally {
    if ($nginxStarted) {
        $ns = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\nginx-stop.ps1') 2>&1
        $nsRc = $LASTEXITCODE
        $ns | Out-File -Encoding utf8 (Join-Path $out '21-nginx-stop.txt')
        Check 'nginx-stop' ($nsRc -eq 0) "exit=$nsRc"
    }
    if ($app) {
        & $py -B (Join-Path $root 'scripts\desktop_input.py') close | Out-Null
        if (-not $app.WaitForExit(20000)) { Stop-Process -Id $app.Id -Force; Check 'app closed by WM_CLOSE' $false 'killed after 20 s' }
        else { Check 'app closed by WM_CLOSE' $true "exit code $($app.ExitCode)" }
        Start-Sleep -Seconds 1
    }
    $left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in 80, 502, 8124, 8125, 18125 })
    Check 'no listener left on 80/502/8124/8125/18125' ($left.Count -eq 0) "left=$($left.Count)"
    $leftProc = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue)
    Check 'no TaidaFlowApp / nginx process left' ($leftProc.Count -eq 0) "left=$($leftProc.Count)"
    $results | Out-File -Encoding utf8 (Join-Path $out '99-summary.txt')
}
Write-Output ("live checks exit={0}" -f $(if ($fail) { 1 } else { 0 }))
if ($fail) { exit 1 } else { exit 0 }

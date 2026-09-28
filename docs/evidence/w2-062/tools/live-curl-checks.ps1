# w2-062 D8: curl checks against a RUNNING TaidaFlow desktop app (+ nginx). Starts nothing, stops nothing.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-062\tools\live-curl-checks.ps1
#            -NginxPort 80 -HttpPort 8124 -MirrorPublicPort 8125 [-ExpectRuntimePort 80] [-NoNginx]
#            [-MirrorClient build\w2-050-mirror-client\mirror_export_client.exe]
# Exit 0 = every check passed.
[CmdletBinding(PositionalBinding = $false)]
param(
    [int]$NginxPort = 80,
    [int]$HttpPort = 8124,
    [int]$MirrorPublicPort = 8125,
    [int]$ExpectRuntimePort = -1,
    [switch]$NoNginx,
    [string]$MirrorClient = ""
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
if ($ExpectRuntimePort -lt 0) { $ExpectRuntimePort = if ($NoNginx) { $MirrorPublicPort } else { $NginxPort } }
$fails = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    if (-not $ok) { $script:fails++ }
    [Console]::Out.WriteLine(("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' })))
}
function RunCurl([string[]]$a) {
    $ErrorActionPreference = 'Continue'
    $o = & curl.exe -s @a 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    return @{ out = $o; rc = $rc }
}
function Head([string]$url, [string[]]$extra = @()) {
    $r = RunCurl (@('-D', '-', '-o', 'NUL', '--max-time', '10') + $extra + @($url))
    $status = ($r.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
    $h = @{}
    foreach ($l in $r.out) { if ($l -match '^([A-Za-z0-9-]+):\s*(.*)$') { $h[$Matches[1].ToLower()] = $Matches[2].Trim() } }
    return @{ status = "$status"; h = $h }
}
function Upgrade([string]$url) {
    # WebSocket handshake only: the 101 status line is printed, then curl stops at --max-time
    $r = RunCurl @('-i', '-N', '--http1.1', '--max-time', '3', '-H', 'Connection: Upgrade', '-H', 'Upgrade: websocket',
                '-H', 'Sec-WebSocket-Version: 13', '-H', 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==',
                '-H', 'Origin: http://127.0.0.1', $url)
    return ($r.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -First 1)
}
$runtimeBody = '{"mirrorPublicPort":' + $ExpectRuntimePort + ',"version":1}'

# app's own HTTP service
$r = Head "http://127.0.0.1:$HttpPort/TaidaFlowApp.html"
Check "GET :$HttpPort/TaidaFlowApp.html -> 200" ($r.status -match ' 200') $r.status
$r = Head "http://127.0.0.1:$HttpPort/runtime.json"
$b = (RunCurl @('--max-time', '5', "http://127.0.0.1:$HttpPort/runtime.json")).out -join ''
Check "GET :$HttpPort/runtime.json -> 200, Cache-Control no-store, $runtimeBody" ($r.status -match ' 200' -and $r.h['cache-control'] -eq 'no-store' -and $b -ceq $runtimeBody) "$($r.status) / $($r.h['cache-control']) / $b"
$s = Upgrade "http://127.0.0.1:$MirrorPublicPort/mirror"
Check "WebSocket upgrade on the relay :$MirrorPublicPort/mirror -> 101" ("$s" -match ' 101') "$s"

if (-not $NoNginx) {
    $base = if ($NginxPort -eq 80) { 'http://127.0.0.1' } else { "http://127.0.0.1:$NginxPort" }
    $r = Head "$base/"
    Check "GET $base/ -> 302 /TaidaFlowApp.html" ($r.status -match ' 302' -and $r.h['location'] -eq '/TaidaFlowApp.html') "$($r.status) $($r.h['location'])"
    $r = Head "$base/TaidaFlowApp.html"
    Check "GET $base/TaidaFlowApp.html -> 200 + COOP/COEP" ($r.status -match ' 200' -and $r.h['cross-origin-opener-policy'] -eq 'same-origin' -and $r.h['cross-origin-embedder-policy'] -eq 'require-corp') $r.status
    $r = Head "$base/TaidaFlowApp.wasm" @('-H', 'Accept-Encoding: gzip')
    Check "GET $base/TaidaFlowApp.wasm (gzip) -> 200 Content-Encoding gzip" ($r.status -match ' 200' -and $r.h['content-encoding'] -eq 'gzip') "$($r.status) $($r.h['content-encoding'])"
    $r = Head "$base/runtime.json" @('-H', 'Accept-Encoding: gzip')
    $b = (RunCurl @('--max-time', '5', "$base/runtime.json")).out -join ''
    Check "GET $base/runtime.json -> 200, Cache-Control no-store, no gzip, no ETag, $runtimeBody" ($r.status -match ' 200' -and $r.h['cache-control'] -eq 'no-store' -and -not $r.h['content-encoding'] -and -not $r.h['etag'] -and $b -ceq $runtimeBody) "$($r.status) / $($r.h['cache-control']) / $b"
    $b = (RunCurl @('--max-time', '5', "$base/api/")).out -join ''
    Check "GET $base/api/ -> REST status" ($b -match '"status"\s*:\s*"ok"') $b
    $s = Upgrade "$base/mirror"
    Check "WebSocket upgrade through nginx $base/mirror -> 101" ("$s" -match ' 101') "$s"
}

if ($MirrorClient) {
    $client = if ([IO.Path]::IsPathRooted($MirrorClient)) { $MirrorClient } else { Join-Path $root $MirrorClient }
    $to = [DateTimeOffset]::Now.ToUnixTimeMilliseconds(); $from = $to - 600 * 1000
    $ws = if ($NoNginx) { "ws://127.0.0.1:$MirrorPublicPort/mirror" } elseif ($NginxPort -eq 80) { 'ws://127.0.0.1/mirror' } else { "ws://127.0.0.1:$NginxPort/mirror" }
    $saved = $env:PATH; $env:PATH = 'C:\Qt\6.8.3\msvc2022_64\bin;' + $saved
    $ErrorActionPreference = 'Continue'
    $mc = & $client --url $ws --origin 'http://127.0.0.1' --session 'w2062live' --from-ms "$from" --to-ms "$to" --timeout-sec 90 2>&1 | ForEach-Object { "$_" }
    $mcRc = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'; $env:PATH = $saved
    $result = $mc | Where-Object { $_ -match 'RESULT ' } | Select-Object -Last 1
    $dl = if ($NoNginx) { $HttpPort } else { $NginxPort }
    Check "history export through $ws, downloadPort = $dl" ($mcRc -eq 0 -and "$result" -match "downloadPort=$dl\b") "$result"
    if ("$result" -match ' url=(\S+)') {
        $url = $Matches[1]
        $link = if ($dl -eq 80) { "http://127.0.0.1$url" } else { "http://127.0.0.1:$dl$url" }
        $r = Head $link
        Check "download $link -> 200 attachment" ($r.status -match ' 200' -and $r.h['content-disposition'] -match 'attachment') "$($r.status) $($r.h['content-disposition'])"
        if (-not $NoNginx) {   # Range / resume is nginx's; the app's own /exports (http.port, fallback) sends the whole file
            $r = Head $link @('-H', 'Range: bytes=0-99')
            Check "download Range bytes=0-99 -> 206" ($r.status -match ' 206') $r.status
        }
    }
}
[Console]::Out.WriteLine("=== $fails check(s) failed")
if ($fails) { exit 1 }
exit 0

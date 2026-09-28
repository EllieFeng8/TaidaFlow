# Live check of a TaidaFlow field package on the DEVELOPMENT PC (w2-057). Not for the plant.
#
# Runs the package the way the plant PC would (start-taidaflow.ps1 or start-taidaflow.bat from the
# package folder, PATH WITHOUT any Qt folder), then checks with curl and logs - no UI interaction:
#   1. scripts\safety_probe.ps1 must say SAFE (192.168.1.201..205:502 unreachable, no COM2, ports
#      free), otherwise nothing is started (exit 3). The field script itself has no probe.
#   2. start (exit code 0 expected), app + nginx listening (80 by default, 502, 8124, 8125, 18125);
#   3. every Qt / MSVC runtime DLL loaded by the app comes from the package folder (load test);
#   4. http://<host>[:port]/ -> 302 /TaidaFlowApp.html, page 200, .wasm with gzip, the same on 8124,
#      WebSocket upgrade 101 on 8125 (/mirror), for 127.0.0.1 and the LAN IPv4;
#      REST API (w2-060) through nginx: GET /api/ (status) and /api/settings/frequency 200 JSON,
#      OPTIONS preflight 200 with Access-Control-Allow-Origin; the internal REST port (-RestPort,
#      default 18080) listens on 127.0.0.1 only and cannot be reached on the LAN address;
#   5. (with a mirror client, dev-only tool) a history CSV export requested like the web page does:
#      downloadPort must equal the nginx port; the link the web page builds (HistoryPage.qml
#      exportDownloadUrl, evaluated with node) is fetched: 200 + same SHA-256 as the file,
#      Range -> 206 via nginx; 8124/exports -> 200;
#   6. app log: no QML module / plugin / library load error;
#   7. the data folder holds TaidaFlowSettings.ini, settings.sqlite, data\, exports\, logs\;
#   8. stop (stop-taidaflow.ps1 / .bat), then no TaidaFlowApp / nginx of ours and no listener left.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-release-package.ps1
#            -Package dist\TaidaFlow-<...> [-Mode ps1|bat] [-UseConfig] [-DataDir build\w2-057-verify-data]
#            [-Port 80] [-RestPort 18080] [-Evidence docs\evidence\w2-057\live-ps1] [-SeedDb <sensor_yyyyMM.sqlite>]
#            [-MirrorClient build\w2-057-mirror-client\mirror_export_client.exe] [-FromMs n -ToMs n]
#   Settings (w2-060): the package's config.json is the site configuration.
#   -Mode ps1 : start-taidaflow.ps1 / stop-taidaflow.ps1 with -DataDir -UseNginx -Port -RestPort on the
#               command line (overrides of config.json for one run) - unless -UseConfig is given.
#   -UseConfig: (always for -Mode bat) the scripts get NO parameters; this check writes the package's
#               config.json with {dataDir = -DataDir, useNginx = true, nginxPort = -Port, restPort =
#               -RestPort} for the run (the original file is kept in <Evidence>\config.json.original
#               and put back at the end, also after an error).
#   -Mode bat : starts <package>\start-taidaflow.bat (TAIDAFLOW_NOPAUSE=1) and stops with
#               stop-taidaflow.bat.
#   -SeedDb   : TEST FIXTURE - copies an existing history database into <data folder>\data before
#               the start (no device on the bench -> no history otherwise). Test data only.
# Exit codes: 0 all checks passed; 1 at least one check failed; 2 bad parameters; 3 probe not SAFE.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [ValidateSet('ps1', 'bat')][string]$Mode = 'ps1',
    [string]$DataDir = "",
    [int]$Port = 80,
    [int]$RestPort = 18080,
    [switch]$UseConfig,
    [string]$Evidence = "",
    [string]$SeedDb = "",
    [string]$MirrorClient = "build\w2-057-mirror-client\mirror_export_client.exe",
    [double]$FromMs = 0,
    [double]$ToMs = 0,
    [int]$TimeoutSec = 120
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $root $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
$Package = Full $Package
if (-not (Test-Path (Join-Path $Package 'TaidaFlowApp.exe'))) { Write-Output "no package at $Package"; exit 2 }
if ($Evidence -eq "") { $Evidence = "docs\evidence\w2-057\live-$Mode" }
$Evidence = Full $Evidence
New-Item -ItemType Directory -Force $Evidence | Out-Null
if ($Mode -eq 'bat') { $UseConfig = [switch]$true }
if ($DataDir -eq "") { $DataDir = Full 'build\w2-057-verify-data' } else { $DataDir = Full $DataDir }
$report = Join-Path $Evidence 'summary.txt'
$script:fails = 0
$lines = New-Object System.Collections.Generic.List[string]
function Note([string]$m) { $lines.Add($m); [Console]::Out.WriteLine($m) }
function Check([string]$what, [bool]$ok, [string]$detail) {
    if (-not $ok) { $script:fails++ }
    Note ("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' }))
}
function Save() { [System.IO.File]::WriteAllLines($report, $lines.ToArray(), (New-Object System.Text.UTF8Encoding($false))) }
$curl = Join-Path $env:SystemRoot 'System32\curl.exe'
function Invoke-Curl([string[]]$a) {
    $ErrorActionPreference = 'Continue'
    $o = & $curl --noproxy '*' -s @a 2>&1 | ForEach-Object { "$_" }
    return [pscustomobject]@{ rc = $LASTEXITCODE; out = @($o) }
}
function HeadInfo([string]$url, [string[]]$extra) {
    $r = Invoke-Curl (@('-o', 'NUL', '-D', '-', '--max-time', '20') + $extra + @($url))
    $status = ($r.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
    return [pscustomobject]@{ rc = $r.rc; status = "$status".Trim(); headers = $r.out }
}
# Any unexpected error after the start: stop what this check started (stop-taidaflow.ps1 only
# acts on the app / nginx recorded in the data folder), write the summary, exit 1.
$script:started = $false
trap {
    Note "EXCEPTION: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))"
    if ($script:started) {
        $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Package 'stop-taidaflow.ps1') -DataDir $DataDir
        $o | ForEach-Object { Note "  cleanup stop | $_" }
    }
    Restore-Config
    Save
    exit 1
}
$pkgConfig = Join-Path $Package 'config.json'
$configBackup = Join-Path $Evidence 'config.json.original'
$script:configWritten = $false
$script:configExisted = $false
function Restore-Config() {
    if (-not $script:configWritten) { return }
    if ($script:configExisted) { Copy-Item -LiteralPath $configBackup -Destination $pkgConfig -Force; Note "package config.json restored from $configBackup" }
    elseif (Test-Path -LiteralPath $pkgConfig) { Remove-Item -LiteralPath $pkgConfig -Force; Note "temporary package config.json removed (there was none before)" }
    $script:configWritten = $false
}
function Hdr($h, [string]$name) { $l = $h.headers | Where-Object { $_ -match "^${name}:" } | Select-Object -Last 1; if ($l) { ($l -split ':', 2)[1].Trim() } else { '' } }

Note "=== verify-release-package $((Get-Date).ToString('o')) mode=$Mode"
Note "package : $Package"
Note "data    : $DataDir"
Note "nginx port: $Port"

# --- 0. nothing of ours may already run; probe ---------------------------------------------------
$pre = @(Get-Process TaidaFlowApp, nginx, Adam60xxSimulator -ErrorAction SilentlyContinue)
foreach ($p in $pre) { Note "already running (NOT touched): $($p.ProcessName) pid $($p.Id) $(try { $p.Path } catch { '' })" }
$probeLog = Join-Path $Evidence 'safety-probe.log'
$probeOut = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\safety_probe.ps1') -Reason "w2-057 verify-release-package ($Mode) before launch" -LogFile $probeLog
$probeRc = $LASTEXITCODE
$probeOut | ForEach-Object { Note "  probe | $_" }
if ($probeRc -ne 0) { Note "safety probe NOT SAFE (exit $probeRc) - nothing started"; Save; exit 3 }
$busy = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in @($Port, 502, 8124, 8125, 18125, $RestPort) })
if ($busy.Count -or @(Get-Process TaidaFlowApp -ErrorAction SilentlyContinue).Count) { Note "ports busy / app running - nothing started"; Save; exit 3 }

# --- 1. data folder (fresh) + optional fixture -----------------------------------------------------
if (Test-Path $DataDir) {
    if (-not $DataDir.StartsWith($root + '\build\', [System.StringComparison]::OrdinalIgnoreCase) -and
        -not $DataDir.StartsWith($root + '\dist\', [System.StringComparison]::OrdinalIgnoreCase)) { Note "refusing to clear $DataDir (not below build\ or dist\)"; Save; exit 2 }
    Remove-Item -LiteralPath $DataDir -Recurse -Force
}
if ($SeedDb -ne "") {
    $SeedDb = Full $SeedDb
    New-Item -ItemType Directory -Force (Join-Path $DataDir 'data') | Out-Null
    Copy-Item -LiteralPath $SeedDb -Destination (Join-Path $DataDir 'data')
    Note "TEST FIXTURE: copied $SeedDb -> $DataDir\data\ (history rows for the export check)"
}

# --- 2. start with a PATH without Qt ----------------------------------------------------------------
$cleanPath = @("$env:SystemRoot\System32", "$env:SystemRoot", "$env:SystemRoot\System32\Wbem", "$env:SystemRoot\System32\WindowsPowerShell\v1.0") -join ';'
$savedPath = $env:PATH
$env:PATH = $cleanPath
Note "PATH for the start: $cleanPath"
$qtOnPath = @($cleanPath -split ';' | Where-Object { Test-Path (Join-Path $_ 'Qt6Core.dll') })
Check 'no Qt6Core.dll on the PATH used for the start' ($qtOnPath.Count -eq 0) ''
$startOut = Join-Path $Evidence 'start.console.txt'
$env:TAIDAFLOW_NOPAUSE = '1'
if ($UseConfig) {
    $script:configExisted = Test-Path -LiteralPath $pkgConfig
    if ($script:configExisted) { Copy-Item -LiteralPath $pkgConfig -Destination $configBackup -Force }
    $json = ([ordered]@{ '_comment' = 'TEMPORARY - written by scripts\verify-release-package.ps1 -UseConfig, restored at its end'
                         dataDir = $DataDir; useNginx = $true; nginxPort = $Port; restPort = $RestPort } | ConvertTo-Json)
    [System.IO.File]::WriteAllText($pkgConfig, $json, (New-Object System.Text.UTF8Encoding($false)))
    $script:configWritten = $true
    Note "package config.json for this run: $($json -replace '\s+', ' ')"
}
$startArgs = if ($UseConfig) { '' } else { ' -DataDir "' + $DataDir + '" -UseNginx -Port ' + $Port + ' -RestPort ' + $RestPort }
if ($Mode -eq 'bat') {
    $cmdLine = '/c ""' + (Join-Path $Package 'start-taidaflow.bat') + '" > "' + $startOut + '" 2>&1"'
} else {
    $cmdLine = '/c ""' + "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" + '" -NoProfile -ExecutionPolicy Bypass -File "' +
               (Join-Path $Package 'start-taidaflow.ps1') + '"' + $startArgs + ' > "' + $startOut + '" 2>&1"'
}
Note "start: cmd.exe $cmdLine"
# ShellExecute (no handle inheritance): the app must not inherit this script's output pipe.
$sp = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList $cmdLine -WindowStyle Hidden -PassThru
$null = $sp.Handle
$script:started = $true
$finished = $sp.WaitForExit($TimeoutSec * 1000)
$env:PATH = $savedPath
Remove-Item Env:\TAIDAFLOW_NOPAUSE -ErrorAction SilentlyContinue
$startRc = if ($finished) { $sp.ExitCode } else { 'TIMEOUT' }
Get-Content -LiteralPath $startOut -ErrorAction SilentlyContinue | ForEach-Object { Note "  start | $_" }
Check "start exit code 0" ("$startRc" -eq '0') "exit $startRc"

$state = $null
try { $state = Get-Content -Raw (Join-Path $DataDir 'taidaflow-app.json') | ConvertFrom-Json } catch { }
$app = if ($state) { Get-Process -Id ([int]$state.pid) -ErrorAction SilentlyContinue } else { $null }
Check 'TaidaFlowApp running (state file pid)' ([bool]$app) $(if ($state) { "pid $($state.pid)" } else { 'no state file' })
if ($app) {
    $listen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in @($Port, 502, 8124, 8125, 18125, $RestPort) })
    foreach ($l in $listen) {
        $n = try { (Get-Process -Id $l.OwningProcess).ProcessName } catch { '?' }
        Note ("  listener {0}:{1} pid {2} ({3})" -f $l.LocalAddress, $l.LocalPort, $l.OwningProcess, $n)
    }
    foreach ($pp in 502, 8124, 8125, 18125) { Check "app listens on $pp" (@($listen | Where-Object { $_.LocalPort -eq $pp -and $_.OwningProcess -eq $app.Id }).Count -gt 0) '' }
    $restListen = @($listen | Where-Object { $_.LocalPort -eq $RestPort -and $_.OwningProcess -eq $app.Id })
    Check "app listens on $RestPort (REST API) on 127.0.0.1 only" ($restListen.Count -gt 0 -and @($restListen | Where-Object { $_.LocalAddress -ne '127.0.0.1' }).Count -eq 0) (($restListen | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ', ')
    $nginxListen = @($listen | Where-Object { $_.LocalPort -eq $Port -and ((Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).ProcessName -eq 'nginx') })
    Check "nginx listens on $Port" ($nginxListen.Count -gt 0) ''

    # --- 3. load test: where do the loaded DLLs come from? ---------------------------------------------
    Start-Sleep -Seconds 3
    $mods = @($app.Modules | ForEach-Object { $_.FileName })
    $pkgMods = @($mods | Where-Object { $_.StartsWith($Package + '\', [System.StringComparison]::OrdinalIgnoreCase) })
    $qtOutside = @($mods | Where-Object { (Split-Path -Leaf $_) -match '^(Qt6|vcruntime|msvcp1|concrt)' -and -not $_.StartsWith($Package + '\', [System.StringComparison]::OrdinalIgnoreCase) })
    $other = @($mods | Where-Object { -not $_.StartsWith($Package + '\', [System.StringComparison]::OrdinalIgnoreCase) -and -not $_.StartsWith($env:SystemRoot + '\', [System.StringComparison]::OrdinalIgnoreCase) })
    [System.IO.File]::WriteAllLines((Join-Path $Evidence 'loaded-modules.txt'), [string[]]$mods)
    Check 'Qt / MSVC runtime DLLs all loaded from the package' ($qtOutside.Count -eq 0) ("{0} module(s) from the package, {1} Qt/MSVC module(s) outside: {2}" -f $pkgMods.Count, $qtOutside.Count, ($qtOutside -join ', '))
    Note ("  modules outside the package and outside {0}: {1}" -f $env:SystemRoot, $(if ($other.Count) { $other -join ', ' } else { 'none' }))
    Note ("  Qt modules loaded: " + (($pkgMods | ForEach-Object { $_.Substring($Package.Length + 1) } | Where-Object { $_ -match 'Qt6|qml\\|platforms\\|styles\\|sqldrivers\\' }) -join ', '))

    # --- 4. HTTP / WebSocket -------------------------------------------------------------------------
    $lan = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' -and $_.PrefixOrigin -ne 'WellKnown' } |
             Sort-Object { if ($_.InterfaceAlias -match 'VMware|vEthernet|Virtual') { 1 } else { 0 } } | Select-Object -First 1 | ForEach-Object { $_.IPAddress })
    $hosts = @('127.0.0.1') + $lan
    Note "hosts checked: $($hosts -join ', ')"
    foreach ($h in $hosts) {
        $base = if ($Port -eq 80) { "http://$h" } else { "http://${h}:$Port" }
        $r = HeadInfo "$base/" @()
        Check "$base/ -> 302 /TaidaFlowApp.html" ($r.status -match ' 302' -and (Hdr $r 'Location') -eq '/TaidaFlowApp.html') "$($r.status), Location: $(Hdr $r 'Location')"
        $r = HeadInfo "$base/TaidaFlowApp.html" @()
        Check "$base/TaidaFlowApp.html -> 200 text/html" ($r.status -match ' 200' -and (Hdr $r 'Content-Type') -match 'text/html') "$($r.status), $(Hdr $r 'Content-Type'), COOP $(Hdr $r 'Cross-Origin-Opener-Policy'), COEP $(Hdr $r 'Cross-Origin-Embedder-Policy')"
        $r = HeadInfo "$base/TaidaFlowApp.wasm" @('-H', 'Accept-Encoding: gzip')
        Check "$base/TaidaFlowApp.wasm (gzip) -> 200 application/wasm, Content-Encoding gzip" ($r.status -match ' 200' -and (Hdr $r 'Content-Encoding') -eq 'gzip' -and (Hdr $r 'Content-Type') -eq 'application/wasm') "$($r.status), $(Hdr $r 'Content-Type'), Content-Encoding $(Hdr $r 'Content-Encoding'), Content-Length $(Hdr $r 'Content-Length')"
        $r = HeadInfo "http://${h}:8124/TaidaFlowApp.html" @()
        Check "http://${h}:8124/TaidaFlowApp.html -> 200 (app)" ($r.status -match ' 200') $r.status
        $ws = Invoke-Curl @('-i', '-N', '--http1.1', '--max-time', '3', '-H', 'Connection: Upgrade', '-H', 'Upgrade: websocket', '-H', 'Sec-WebSocket-Version: 13',
                     '-H', 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==', '-H', "Origin: $base", "http://${h}:8125/mirror")
        $wsStatus = ($ws.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -First 1)
        Check "ws://${h}:8125/mirror upgrade -> 101" ("$wsStatus" -match ' 101') "$wsStatus (curl exit $($ws.rc): 28 = closed by --max-time after the upgrade)"
        # w2-060: REST API through nginx
        $api = Invoke-Curl @('-D', '-', '--max-time', '20', "$base/api/")
        $apiStatus = ($api.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
        Check "$base/api/ (REST status via nginx) -> 200 {`"status`":...}" ("$apiStatus" -match ' 200' -and (($api.out -join "`n") -match '"status"')) "$apiStatus $(($api.out | Select-Object -Last 1))"
        $api = Invoke-Curl @('-D', '-', '--max-time', '20', "$base/api/settings/frequency")
        $apiStatus = ($api.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
        $acao = ($api.out | Where-Object { $_ -match '^Access-Control-Allow-Origin:' } | Select-Object -Last 1)
        Check "$base/api/settings/frequency -> 200 read_frequency, CORS header" ("$apiStatus" -match ' 200' -and (($api.out -join "`n") -match '"read_frequency"') -and "$acao" -match '\*') "$apiStatus $(($api.out | Select-Object -Last 1)) $acao"
        $pre2 = Invoke-Curl @('-X', 'OPTIONS', '-D', '-', '-o', 'NUL', '--max-time', '20', '-H', 'Origin: http://example.invalid', '-H', 'Access-Control-Request-Method: PUT', '-H', 'Access-Control-Request-Headers: Content-Type', "$base/api/settings/frequency")
        $preStatus = ($pre2.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
        $preMethods = ($pre2.out | Where-Object { $_ -match '^Access-Control-Allow-Methods:' } | Select-Object -Last 1)
        Check "OPTIONS $base/api/settings/frequency (preflight) -> 200, Allow-Methods has PUT" ("$preStatus" -match ' 200' -and "$preMethods" -match 'PUT') "$preStatus $preMethods"
        if ($h -ne '127.0.0.1') {
            $direct = Invoke-Curl @('-o', 'NUL', '-w', '%{http_code}', '--connect-timeout', '3', '--max-time', '5', "http://${h}:$RestPort/")
            Check "internal REST port not reachable on the LAN address (http://${h}:$RestPort/)" ($direct.rc -ne 0) "curl exit $($direct.rc) (7 = connection refused), http_code $($direct.out -join '')"
        }
    }

    # --- 5. export via the mirror + download links -----------------------------------------------------
    $client = Full $MirrorClient
    if (-not (Test-Path $client) -and -not $PSBoundParameters.ContainsKey('MirrorClient')) {
        # same source, built by docs\evidence\w2-050\tools\build-mirror-client.bat
        $client = Full 'build\w2-050-mirror-client\mirror_export_client.exe'
    }
    if (Test-Path $client) {
        if ($ToMs -le 0) { $ToMs = [double][DateTimeOffset]::Now.ToUnixTimeMilliseconds() }
        if ($FromMs -le 0) { $FromMs = $ToMs - 3600 * 1000 }
        $env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;" + $savedPath      # the dev-only client uses the Qt install
        $ErrorActionPreference = 'Continue'
        $mc = & $client --url "ws://127.0.0.1:8125/mirror" --origin "http://127.0.0.1" --session "w2057verify" --from-ms "$FromMs" --to-ms "$ToMs" --timeout-sec 90 2>&1 | ForEach-Object { "$_" }
        $mcRc = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        $env:PATH = $savedPath
        [System.IO.File]::WriteAllLines((Join-Path $Evidence 'mirror-export.txt'), [string[]]$mc)
        $result = $mc | Where-Object { $_ -match 'RESULT ' } | Select-Object -Last 1
        Note "  mirror client exit $mcRc : $result"
        Check "history export via the mirror done, downloadPort = $Port" ($mcRc -eq 0 -and "$result" -match "downloadPort=$Port\b") "$result"
        if ("$result" -match 'fileName=(\S+)') {
            $file = $Matches[1]
            $url = if ("$result" -match ' url=(\S+)') { $Matches[1] } else { "/exports/$file" }
            $disk = Join-Path $DataDir "exports\$file"
            $diskHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $disk).Hash
            $diskLen = (Get-Item -LiteralPath $disk).Length
            Note "  export file: $disk ($diskLen bytes, SHA-256 $diskHash)"
            # The link exactly as the web page builds it (HistoryPage.qml exportDownloadUrl), evaluated with node.
            $qml = [System.IO.File]::ReadAllText((Join-Path $root 'TaidaFlowContent\HistoryPage.qml'))
            $fn = [regex]::Match($qml, '(?s)function exportDownloadUrl\(entry\) \{.*?\n    \}').Value
            $node = 'C:\tools\emsdk\node\16.20.0_64bit\bin\node.exe'
            foreach ($h in $hosts) {
                $js = "var Td = { pageHost: '$h' };`n$fn`nconsole.log(exportDownloadUrl({ url: '$url', downloadPort: $Port }));"
                $jsFile = Join-Path $Evidence 'exportDownloadUrl.js'
                [System.IO.File]::WriteAllText($jsFile, $js)
                $link = if ((Test-Path $node) -and $fn) { (& $node $jsFile | Select-Object -First 1) } else { '' }
                Check "web page link for host $h" ("$link" -eq "http://${h}:$Port$url") "$link (HistoryPage.qml exportDownloadUrl via node)"
                if ($link) {
                    $tmp = Join-Path $Evidence "download-$($h -replace '\.', '_').csv"
                    $d = Invoke-Curl @('-o', $tmp, '-D', '-', '--max-time', '60', $link)
                    $st = ($d.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
                    $ok = (Test-Path $tmp) -and ((Get-FileHash -Algorithm SHA256 -LiteralPath $tmp).Hash -eq $diskHash)
                    Check "GET $link -> 200, same SHA-256 as the file" ("$st" -match ' 200' -and $ok) "$st, server $(($d.out | Where-Object { $_ -match '^Server:' }) -join '')"
                    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
                    $rr = HeadInfo $link @('-H', 'Range: bytes=0-99')
                    Check "Range bytes=0-99 on $link -> 206" ($rr.status -match ' 206' -and (Hdr $rr 'Content-Range') -eq "bytes 0-99/$diskLen") "$($rr.status), Content-Range $(Hdr $rr 'Content-Range')"
                    $noPort = $link -replace ":80/", "/"
                    if ($Port -eq 80 -and $noPort -ne $link) {
                        $r2 = HeadInfo $noPort @()
                        Check "same file without :80 ($noPort) -> 200" ($r2.status -match ' 200') $r2.status
                    }
                }
            }
            $r8124 = HeadInfo "http://127.0.0.1:8124$url" @()
            Check "fallback http://127.0.0.1:8124$url -> 200" ($r8124.status -match ' 200') $r8124.status
        }
    } else { Note "  (no mirror client at $client - export check skipped)" }

    # --- 6. app log ----------------------------------------------------------------------------------
    $appLog = [string]$state.appLog
    $logText = if (Test-Path $appLog) { Get-Content -LiteralPath $appLog } else { @() }
    Copy-Item -LiteralPath $appLog -Destination (Join-Path $Evidence 'app.log') -ErrorAction SilentlyContinue
    $bad = @($logText | Where-Object { $_ -match '(?i)module "[^"]+" is not installed|is not a type|Cannot load library|could not find the Qt platform plugin|Failed to load|plugin .* could not be loaded|QQmlApplicationEngine failed' })
    Check 'app log: no QML module / plugin / library load error' ($bad.Count -eq 0) ("{0} line(s), {1} bytes{2}" -f $logText.Count, $(if (Test-Path $appLog) { (Get-Item $appLog).Length } else { 0 }), $(if ($bad.Count) { ': ' + ($bad -join ' | ') } else { '' }))
    $warn = @($logText | Select-Object -First 25)
    $warn | ForEach-Object { Note "  app.log | $_" }
}

# --- 7. data folder ---------------------------------------------------------------------------------
$listing = @(Get-ChildItem -LiteralPath $DataDir -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
    "{0}{1}" -f $_.FullName.Substring($DataDir.Length + 1), $(if ($_.PSIsContainer) { '\' } else { " ($($_.Length) bytes)" }) })
[System.IO.File]::WriteAllLines((Join-Path $Evidence 'datadir-listing.txt'), [string[]]$listing)
foreach ($want in 'TaidaFlowSettings.ini', 'settings.sqlite', 'data', 'exports', 'logs') {
    Check "data folder has $want" (Test-Path (Join-Path $DataDir $want)) ''
}

# --- 8. stop -----------------------------------------------------------------------------------------
$stopOut = Join-Path $Evidence 'stop.console.txt'
$env:TAIDAFLOW_NOPAUSE = '1'
if ($Mode -eq 'bat') { $cmdLine = '/c ""' + (Join-Path $Package 'stop-taidaflow.bat') + '" > "' + $stopOut + '" 2>&1"' }
else {
    $stopArgs = if ($UseConfig) { '' } else { ' -DataDir "' + $DataDir + '"' }
    $cmdLine = '/c ""' + "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" + '" -NoProfile -ExecutionPolicy Bypass -File "' +
               (Join-Path $Package 'stop-taidaflow.ps1') + '"' + $stopArgs + ' > "' + $stopOut + '" 2>&1"'
}
$tp = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList $cmdLine -WindowStyle Hidden -PassThru
$null = $tp.Handle
$stopRc = if ($tp.WaitForExit($TimeoutSec * 1000)) { $tp.ExitCode } else { 'TIMEOUT' }
Remove-Item Env:\TAIDAFLOW_NOPAUSE -ErrorAction SilentlyContinue
Get-Content -LiteralPath $stopOut -ErrorAction SilentlyContinue | ForEach-Object { Note "  stop | $_" }
Check 'stop exit code 0' ("$stopRc" -eq '0') "exit $stopRc"
Start-Sleep -Seconds 1
$leftProc = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue | Where-Object { -not ($pre | Where-Object Id -eq $_.Id) })
Check 'no TaidaFlowApp / nginx started by this check left' ($leftProc.Count -eq 0) (($leftProc | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')
$leftListen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in @($Port, 502, 8124, 8125, 18125, $RestPort) })
Check "no listener left on $Port/502/8124/8125/18125/$RestPort" ($leftListen.Count -eq 0) (($leftListen | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" }) -join ', ')

if ($leftProc.Count -eq 0 -and (Test-Path $DataDir) -and $DataDir.StartsWith($Package + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    # Test data inside the package folder must not stay there.
    Remove-Item -LiteralPath $DataDir -Recurse -Force
    Note "test data folder removed from the package: $DataDir"
}
Restore-Config
Note ("=== {0} check(s) failed" -f $script:fails)
Save
if ($script:fails -gt 0) { exit 1 }
exit 0

# Live check of a TaidaFlow field package on the DEVELOPMENT PC (w2-057, w2-062, w2-065). Not for the plant.
#
# Runs the package the way the plant PC would (start-taidaflow.ps1 or .bat from the package folder,
# PATH WITHOUT any Qt folder, NO config.json in the package), then checks with curl and logs - no UI:
#   0. package as shipped: no config.json, bundled nginx (nginx\nginx.exe, docs\LICENSE,
#      conf\mime.types, no conf\nginx.conf), no path of the build machine in any text file or in
#      TaidaFlowApp.exe (Mango A9); scripts\safety_probe.ps1 SAFE for the app defaults
#      (192.168.1.201..205:502 unreachable, no COM2, ports free), otherwise nothing is started (exit 3).
#   A. FIRST START (nginx off): start -DataDir <data> -NoNginx. config.json is CREATED in the package
#      by TaidaFlowApp.exe --write-default-config (same bytes as a fresh default file) and the data
#      folder is created. The default dataDir is C:\TaidaFlowData - on this development PC the check
#      replaces it for this start only with the one-time override -DataDir (default
#      build\verify-release-data). App ports up, page + runtime.json (no-store, mirrorPublicPort =
#      mirror.publicPort because nginx is off) on http.port, WebSocket 101 on mirror.publicPort.
#      w2-065 logs: <data>\logs (log.dir default "logs" relative to dataDir) holds the app's own
#      taidaflow-<today>.log (only warning/critical/fatal lines) and taidaflow-<today>-full.log (with info
#      lines) and launcher-<today>.log of the start script; no taidaflow-yyyyMMdd-HHmmss.log is written. Stop.
#   B. CONFIGURED SITE (nginx on, Mango A6/A7): the check edits config.json like the plant engineer
#      does (only "dataDir" = <data>), runs scripts\install-nginx-config.ps1 (writes nginx\conf\nginx.conf
#      with the web root relative to the nginx folder, nginx -t passes), then the start script without
#      options: it starts the bundled nginx like "start nginx" and the app. Checks through nginx for
#      127.0.0.1 and the LAN IPv4: "/" 302, page 200, .wasm gzip, /runtime.json no-store =
#      {"mirrorPublicPort":<nginx.port>,...} (also on http.port), WebSocket 101 on nginx /mirror and on
#      the fallback relay, REST /api/ (status, frequency, CORS, OPTIONS), loopback-only ports refused
#      on the LAN address, DLLs loaded from the package, app log without load errors; with the dev-only
#      mirror client a history CSV export through nginx /mirror: downloadPort = nginx.port, the page's
#      link (HistoryPage.qml exportDownloadUrl via node) 200 + same SHA-256, Range 206. Stop: the app
#      is closed, nginx keeps running (checked), then "nginx -s quit" from nginx\ (as documented).
#      w2-065: nginx writes <data>\logs\nginx-access-<today>.log (the requests of this check) and
#      nginx-error.log, not nginx\logs\access.log; the app's full log has no load error; stop writes to
#      launcher-<today>.log.
#   C. MOVED INSTALLATION (Mango A9, only -Mode ps1): the whole package folder (with config.json and
#      nginx.conf of B) is copied to build\verify-release-moved\<name>; its start script must detect
#      that nginx.conf belongs to another folder, regenerate it (nginx -t passes), start nginx from the
#      new folder and serve the page / runtime.json / /mirror; then stop + nginx -s quit; the copy is
#      deleted.
#   D. config.json that is not valid JSON: start script exit 2 and TaidaFlowApp.exe itself exit 2
#      (QT_QPA_PLATFORM=offscreen with the plugin of the Qt installation - the package ships only
#      qwindows - and TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS), file unchanged, nothing
#      listening. w2-065: the app logs the failure in the fallback folder <package>\logs (folder of
#      config.json): taidaflow-<today>-full.log / .log with the JSON error and the exit-code-2 line.
#   E. the package is left as shipped (no config.json, no nginx.conf / nginx logs / temp, no logs\) and nothing of ours
#      runs.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-release-package.ps1
#            -Package dist\TaidaFlow-<...> [-Mode ps1|bat] [-DataDir build\verify-release-data]
#            [-Evidence <folder>] [-SeedDb <sensor_yyyyMM.sqlite>]
#            [-MirrorClient build\w2-050-mirror-client\mirror_export_client.exe] [-FromMs n -ToMs n]
#   -SeedDb : TEST FIXTURE - copies a history database into <data folder>\data before phase B (no device
#             on the bench -> no history otherwise). Test data only.
# Exit codes: 0 all checks passed; 1 at least one check failed; 2 bad parameters; 3 probe not SAFE or
# something of ours already running.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [ValidateSet('ps1', 'bat')][string]$Mode = 'ps1',
    [string]$DataDir = "",
    [string]$Evidence = "",
    [string]$SeedDb = "",
    [string]$MirrorClient = "build\w2-050-mirror-client\mirror_export_client.exe",
    [double]$FromMs = 0,
    [double]$ToMs = 0,
    [int]$TimeoutSec = 120
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $root $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
$Package = Full $Package
$pkgExe = Join-Path $Package 'TaidaFlowApp.exe'
if (-not (Test-Path $pkgExe)) { Write-Output "no package at $Package"; exit 2 }
if ($Evidence -eq "") { $Evidence = "build\verify-release-evidence\$Mode" }
$Evidence = Full $Evidence
New-Item -ItemType Directory -Force $Evidence | Out-Null
if ($DataDir -eq "") { $DataDir = Full 'build\verify-release-data' } else { $DataDir = Full $DataDir }
if (-not $DataDir.StartsWith($root + '\build\', [System.StringComparison]::OrdinalIgnoreCase)) { Write-Output "-DataDir must be below $root\build\"; exit 2 }
$pkgConfig = Join-Path $Package 'config.json'
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
function Hdr($h, [string]$name) { $l = $h.headers | Where-Object { $_ -match "^${name}:" } | Select-Object -Last 1; if ($l) { ($l -split ':', 2)[1].Trim() } else { '' } }
function Get-Sha([string]$p) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash }
$cleanPath = @("$env:SystemRoot\System32", "$env:SystemRoot", "$env:SystemRoot\System32\Wbem", "$env:SystemRoot\System32\WindowsPowerShell\v1.0") -join ';'
$ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
# Runs a command line through cmd.exe with the Qt-free PATH (ShellExecute: no handle inheritance, so a
# started app / nginx does not keep this script's output pipe). Returns the exit code or 'TIMEOUT'.
function Invoke-Cmd([string]$cmdLine, [string]$outFile) {
    $saved = $env:PATH
    $env:PATH = $cleanPath
    $env:TAIDAFLOW_NOPAUSE = '1'
    Remove-Item Env:\TAIDAFLOW_CONFIG -ErrorAction SilentlyContinue
    try {
        $full = '/c "' + $cmdLine + ' > "' + $outFile + '" 2>&1"'
        $p = Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList $full -WindowStyle Hidden -PassThru
        $null = $p.Handle
        $rc = if ($p.WaitForExit($TimeoutSec * 1000)) { $p.ExitCode } else { 'TIMEOUT' }
    } finally { $env:PATH = $saved; Remove-Item Env:\TAIDAFLOW_NOPAUSE -ErrorAction SilentlyContinue }
    Get-Content -LiteralPath $outFile -ErrorAction SilentlyContinue | ForEach-Object { Note "  | $_" }
    return $rc
}
function Start-Cmd([string]$folder, [string]$extra) {
    if ($Mode -eq 'bat') { return '"' + (Join-Path $folder 'start-taidaflow.bat') + '" ' + $extra }
    return '"' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $folder 'start-taidaflow.ps1') + '" ' + $extra
}
function Stop-Cmd([string]$folder, [string]$extra) {
    if ($Mode -eq 'bat') { return '"' + (Join-Path $folder 'stop-taidaflow.bat') + '" ' + $extra }
    return '"' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $folder 'stop-taidaflow.ps1') + '" ' + $extra
}
# nginx.exe of a package folder = its config.json nginx.exe (w2-065; relative = to the folder of config.json,
# default nginx\nginx.exe); a missing or broken config.json -> the app's default. Running instances,
# "nginx -s quit" in its folder (like the documentation).
function Get-NginxExeOf([string]$folder) {
    $c = Get-TaidaFlowConfig -Path (Join-Path $folder 'config.json') -Exe (Join-Path $folder 'TaidaFlowApp.exe')
    if ($c.Error) { $c = Get-TaidaFlowConfig -Path (Join-Path $folder 'no-such-config.json') -Exe (Join-Path $folder 'TaidaFlowApp.exe') }
    return (Resolve-TaidaFlowNginxExe $c)
}
function Get-NginxOf([string]$folder) {
    $exe = Get-NginxExeOf $folder
    return @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { try { [string]::Equals($_.Path, $exe, [System.StringComparison]::OrdinalIgnoreCase) } catch { $false } })
}
function Stop-NginxOf([string]$folder, [string]$what) {
    $nDir = Split-Path -Parent (Get-NginxExeOf $folder)
    $procs = Get-NginxOf $folder
    if (-not $procs.Count) { return }
    $procs | ForEach-Object { $null = $_.Handle }
    $q = Start-Process -FilePath (Join-Path $nDir 'nginx.exe') -ArgumentList @('-s', 'quit') -WorkingDirectory $nDir -WindowStyle Hidden -PassThru
    $null = $q.Handle
    $null = $q.WaitForExit(20000)
    $deadline = (Get-Date).AddSeconds(20)
    while (@($procs | Where-Object { -not $_.HasExited }).Count -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    Check "$what : 'nginx -s quit' in nginx\ stops the bundled nginx" (@($procs | Where-Object { -not $_.HasExited }).Count -eq 0) "exit $($q.ExitCode)"
}
$script:running = $null      # package folder whose app / nginx this check may have started
trap {
    Note "EXCEPTION: $($_.Exception.Message) (line $($_.InvocationInfo.ScriptLineNumber))"
    if ($script:running) {
        $null = Invoke-Cmd (Stop-Cmd $script:running ('-DataDir "' + $DataDir + '"')) (Join-Path $Evidence 'cleanup-stop.console.txt')
        Stop-NginxOf $script:running 'cleanup'
    }
    Save
    exit 1
}
# w2-065: the log files of one start in the log folder (config.json log.dir resolved against dataDir).
function Test-LogFolder([string]$logDir, [string]$phase, [switch]$Launcher) {
    $today = (Get-Date).ToString('yyyy-MM-dd')
    $quiet = Join-Path $logDir "taidaflow-$today.log"; $full = Join-Path $logDir "taidaflow-$today-full.log"
    $listing = @(Get-ChildItem -LiteralPath $logDir -File -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Name) ($($_.Length) bytes)" })
    Note "  $phase log folder $logDir : $($listing -join ', ')"
    $fullText = if (Test-Path -LiteralPath $full) { @(Get-Content -LiteralPath $full -Encoding UTF8) } else { @() }
    $quietText = if (Test-Path -LiteralPath $quiet) { @(Get-Content -LiteralPath $quiet -Encoding UTF8) } else { $null }
    $infoLines = @($fullText | Where-Object { $_ -match '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[info\] ' }).Count
    Check "$phase app log (full) taidaflow-$today-full.log written by the app, with [info] lines" ($infoLines -gt 0) "$($fullText.Count) line(s), $infoLines info"
    Check "$phase full log has this start's config.json lines ([Config] ... log.dir (resolved) = $logDir)" (@($fullText | Where-Object { $_.Contains('log.dir (resolved) = ' + $logDir) }).Count -gt 0) ''
    $quietBad = @()
    if ($null -ne $quietText) { $quietBad = @($quietText | Where-Object { $_ -match '^\d{4}-\d{2}-\d{2} ' -and $_ -notmatch '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[(warning|critical|fatal)\] ' }) }
    $quietDetail = if ($null -ne $quietText) { "$($quietText.Count) line(s)" } else { 'missing' }
    if ($quietBad.Count) { $quietDetail += ' - ' + (($quietBad | Select-Object -First 3) -join ' | ') }
    Check "$phase app log (quiet) taidaflow-$today.log exists, only warning / critical / fatal lines" ($null -ne $quietText -and $quietBad.Count -eq 0) $quietDetail
    $legacy = @(Get-ChildItem -LiteralPath $logDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^taidaflow-\d{8}-\d{6}\.log(\.stdout)?$' })
    Check "$phase no taidaflow-yyyyMMdd-HHmmss.log (the start script no longer redirects the app's output)" ($legacy.Count -eq 0) (($legacy | ForEach-Object { $_.Name }) -join ', ')
    if ($Launcher) {
        $ll = Join-Path $logDir "launcher-$today.log"
        $lt = if (Test-Path -LiteralPath $ll) { (Get-Content -LiteralPath $ll -Encoding UTF8) -join "`n" } else { '' }
        Check "$phase launcher-$today.log in the log folder with the start lines" ($lt -match '\[start\] === TaidaFlow FIELD start' -and $lt -match '\[start\] started \(exit 0\)') ''
        Check "$phase no launcher.log, no QT_LOGGING_CONF any more" (-not (Test-Path -LiteralPath (Join-Path $logDir 'launcher.log')) -and $lt -notmatch 'QT_LOGGING_CONF=') ''
        Copy-Item -LiteralPath $ll -Destination (Join-Path $Evidence ($phase.TrimEnd('.') + '-' + (Split-Path -Leaf $ll))) -Force -ErrorAction SilentlyContinue
    }
    foreach ($x in $quiet, $full) { if (Test-Path -LiteralPath $x) { Copy-Item -LiteralPath $x -Destination (Join-Path $Evidence ($phase.TrimEnd('.') + '-' + (Split-Path -Leaf $x))) -Force } }
}
function Get-State { try { return (Get-Content -Raw (Join-Path $DataDir 'taidaflow-app.json') | ConvertFrom-Json) } catch { return $null } }
function Test-AppPorts($cfgV, $app, [string]$phase) {
    $want = @([int]$cfgV['modbusServer.port'], [int]$cfgV['http.port'], [int]$cfgV['mirror.publicPort'], [int]$cfgV['mirror.internalPort'], [int]$cfgV['rest.port'])
    $listen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $app.Id -and $want -contains $_.LocalPort })
    foreach ($l in $listen) { Note ("  listener {0}:{1} pid {2}" -f $l.LocalAddress, $l.LocalPort, $l.OwningProcess) }
    foreach ($pp in $want) { Check "$phase app listens on $pp" (@($listen | Where-Object { $_.LocalPort -eq $pp }).Count -gt 0) '' }
    foreach ($lo in @([int]$cfgV['mirror.internalPort'], [int]$cfgV['rest.port'])) {
        $x = @($listen | Where-Object { $_.LocalPort -eq $lo })
        Check "$phase port $lo only on 127.0.0.1" ($x.Count -gt 0 -and @($x | Where-Object { $_.LocalAddress -ne '127.0.0.1' }).Count -eq 0) (($x | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" }) -join ', ')
    }
}
function Test-Runtime([string]$url, [string]$expected, [string]$phase) {
    $rt = Invoke-Curl @('-D', '-', '--max-time', '20', '-H', 'Accept-Encoding: gzip', $url)
    $st = ($rt.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
    $cc = ($rt.out | Where-Object { $_ -match '^Cache-Control:' } | Select-Object -Last 1)
    $ct = ($rt.out | Where-Object { $_ -match '^Content-Type:' } | Select-Object -Last 1)
    $body = ($rt.out | Select-Object -Last 1)
    Check "$phase $url -> 200 application/json, no-store, $expected" ("$st" -match ' 200' -and "$cc" -match 'no-store' -and "$ct" -match 'application/json' -and "$body" -ceq $expected) "$st | $ct | $cc | $body"
}
function Test-Ws([string]$url, [string]$origin, [string]$phase) {
    $ws = Invoke-Curl @('-i', '-N', '--http1.1', '--max-time', '3', '-H', 'Connection: Upgrade', '-H', 'Upgrade: websocket', '-H', 'Sec-WebSocket-Version: 13',
                 '-H', 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==', '-H', "Origin: $origin", $url)
    $st = ($ws.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -First 1)
    Check "$phase $($url -replace '^http', 'ws') upgrade -> 101" ("$st" -match ' 101') "$st (curl exit $($ws.rc): 28 = closed by --max-time after the upgrade)"
}
function Wait-NoListeners([int[]]$ports, [string]$what) {
    Start-Sleep -Seconds 1
    $left = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
    Check "$what : no listener left on $($ports -join '/')" ($left.Count -eq 0) (($left | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" }) -join ', ')
}
# Build-machine paths (Mango A9): repository, its parent, the user profile - in text files and the exe.
function Test-NoMachinePaths([string]$folder, [string]$phase) {
    $machinePaths = @($root, (Split-Path -Parent $root), $env:USERPROFILE) | Where-Object { $_ } | ForEach-Object { @($_, ($_ -replace '\\', '/')) }
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $hits = New-Object System.Collections.Generic.List[string]
    $n = 0
    foreach ($f in @(Get-ChildItem -LiteralPath $folder -Recurse -File)) {
        $isExe = $f.Name -eq 'TaidaFlowApp.exe'
        if (-not $isExe -and ($f.Length -gt 20MB -or $f.Extension -match '^\.(exe|dll|wasm|gz|png|jpg|ico|ttf|otf|woff2?|qmlc)$')) { continue }
        $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
        if (-not $isExe -and [Array]::IndexOf($bytes, [byte]0) -ge 0) { continue }
        $n++
        $text = $latin1.GetString($bytes)
        foreach ($m in $machinePaths) {
            $u16 = $latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($m))
            if ($text.IndexOf($m, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or ($isExe -and $text.IndexOf($u16, [System.StringComparison]::OrdinalIgnoreCase) -ge 0)) {
                $hits.Add("$($f.FullName.Substring($folder.Length + 1)): $m")
            }
        }
    }
    Check "$phase no build-machine path ($($machinePaths -join ' | ')) in $n text file(s) + TaidaFlowApp.exe" ($hits.Count -eq 0) ($hits -join '; ')
}

Note "=== verify-release-package $((Get-Date).ToString('o')) mode=$Mode"
Note "package   : $Package"
Note "data      : $DataDir"

# --- 0. package as shipped, nothing of ours running, probe --------------------------------------------
$pre = @(Get-Process TaidaFlowApp, nginx, Adam60xxSimulator -ErrorAction SilentlyContinue)
foreach ($p in $pre) { Note "already running (NOT touched): $($p.ProcessName) pid $($p.Id) $(try { $p.Path } catch { '' })" }
if (@($pre | Where-Object { $_.ProcessName -ne 'Adam60xxSimulator' }).Count) { Note "TaidaFlowApp / nginx already running - nothing started"; Save; exit 3 }
Check 'package contains no config.json (never shipped)' (-not (Test-Path $pkgConfig)) $pkgConfig
if (Test-Path $pkgConfig) { Note "remove $pkgConfig first (left over by an earlier run?)"; Save; exit 2 }
foreach ($want in 'nginx\nginx.exe', 'nginx\docs\LICENSE', 'nginx\conf\mime.types', 'nginx\SOURCE.txt', 'scripts\install-nginx-config.ps1', 'scripts\taidaflow-config.ps1', 'deploy\nginx\taidaflow.conf') {
    Check "package has $want" (Test-Path (Join-Path $Package $want) -PathType Leaf) ''
}
Check 'package has no nginx\conf\nginx.conf (written on the plant PC)' (-not (Test-Path (Join-Path $Package 'nginx\conf\nginx.conf'))) ''
Check 'package has no logging\quiet.ini and no logs\ (w2-065: the app writes its own log files)' (-not (Test-Path (Join-Path $Package 'logging')) -and -not (Test-Path (Join-Path $Package 'logs'))) ''
Test-NoMachinePaths $Package '0.'
$defaults = Get-TaidaFlowConfig -Path $pkgConfig -Exe $pkgExe
$dv = $defaults.Values
Check 'default config.json: nginx.exe nginx\nginx.exe = the bundled nginx of the package (w2-065 D3)' ((Resolve-TaidaFlowNginxExe $defaults) -eq (Join-Path $Package 'nginx\nginx.exe') -and $defaults.Sources['nginx.exe'] -eq 'default') "$($dv['nginx.exe']) -> $(Resolve-TaidaFlowNginxExe $defaults)"
Check 'default config.json: log.dir logs, quiet on 60 days, full on 7 days (TaidaFlowApp.exe --write-default-config)' ($dv['log.dir'] -eq 'logs' -and $dv['log.quiet.enabled'] -eq $true -and [int]$dv['log.quiet.keepDays'] -eq 60 -and $dv['log.full.enabled'] -eq $true -and [int]$dv['log.full.keepDays'] -eq 7) ''
$allPorts = @('modbusServer.port', 'http.port', 'mirror.publicPort', 'mirror.internalPort', 'rest.port', 'nginx.port') | ForEach-Object { [int]$dv[$_] }
$probeOut = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\safety_probe.ps1') -Reason "verify-release-package ($Mode) before launch" -LogFile (Join-Path $Evidence 'safety-probe.log') -Config $pkgConfig -Exe $pkgExe
$probeRc = $LASTEXITCODE
$probeOut | ForEach-Object { Note "  probe | $_" }
if ($probeRc -ne 0) { Note "safety probe NOT SAFE (exit $probeRc) - nothing started"; Save; exit 3 }
if (@(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $allPorts -contains $_.LocalPort }).Count) { Note "ports busy - nothing started"; Save; exit 3 }
if (Test-Path $DataDir) { Remove-Item -LiteralPath $DataDir -Recurse -Force }
Check 'data folder does not exist before the first start' (-not (Test-Path $DataDir)) $DataDir
$defaultRef = Join-Path $Evidence 'reference-default-config.json'
Remove-Item -LiteralPath $defaultRef -Force -ErrorAction SilentlyContinue
$w = Invoke-TaidaFlowWriteDefaultConfig $pkgExe $defaultRef
Check 'reference default file from the package exe (--write-default-config, exit 0)' ($w.rc -eq 0) "exit $($w.rc): $($w.output)"
Note "PATH for every start: $cleanPath"
Check 'no Qt6Core.dll on the PATH used for the starts' (@($cleanPath -split ';' | Where-Object { Test-Path (Join-Path $_ 'Qt6Core.dll') }).Count -eq 0) ''

# --- A. first start, nginx off -------------------------------------------------------------------------
Note "--- A. first start (no config.json), one-time -DataDir, -NoNginx"
$script:running = $Package
$rc = Invoke-Cmd (Start-Cmd $Package ('-DataDir "' + $DataDir + '" -NoNginx')) (Join-Path $Evidence 'A-start.console.txt')
Check 'A. start exit code 0' ("$rc" -eq '0') "exit $rc"
Check 'A. config.json created in the package folder at the first start' (Test-Path $pkgConfig -PathType Leaf) $pkgConfig
if (Test-Path $pkgConfig) {
    Copy-Item -LiteralPath $pkgConfig -Destination (Join-Path $Evidence 'A-created-config.json') -Force
    Check 'A. created config.json = the app default file (same SHA-256 as --write-default-config)' ((Get-Sha $pkgConfig) -eq (Get-Sha $defaultRef)) (Get-Sha $pkgConfig)
    $bytes = [System.IO.File]::ReadAllBytes($pkgConfig)
    Check 'A. created config.json: UTF-8 without BOM, dataDir C:\TaidaFlowData' (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF) -and ([System.Text.Encoding]::UTF8.GetString($bytes) -match '"dataDir": "C:\\\\TaidaFlowData"')) ''
}
Check 'A. data folder created' (Test-Path $DataDir -PathType Container) $DataDir
$eff = Join-Path $DataDir 'config.effective.json'
Check 'A. one-time overrides passed as <data folder>\config.effective.json' (Test-Path $eff -PathType Leaf) $eff
if (Test-Path $eff) { Copy-Item -LiteralPath $eff -Destination (Join-Path $Evidence 'A-config.effective.json') -Force }
$state = Get-State
$app = if ($state) { Get-Process -Id ([int]$state.pid) -ErrorAction SilentlyContinue } else { $null }
Check 'A. TaidaFlowApp running (state file pid)' ([bool]$app) $(if ($state) { "pid $($state.pid)" } else { 'no state file' })
Check 'A. nginx not started (-NoNginx)' ((Get-NginxOf $Package).Count -eq 0) ''
if ($app) {
    Test-AppPorts $dv $app 'A.'
    $expA = Get-TaidaFlowRuntimeJson ([int]$dv['mirror.publicPort'])
    Test-Runtime "http://127.0.0.1:$($dv['http.port'])/runtime.json" $expA 'A. (nginx off -> mirror.publicPort)'
    $r = HeadInfo "http://127.0.0.1:$($dv['http.port'])/TaidaFlowApp.html" @()
    Check "A. http://127.0.0.1:$($dv['http.port'])/TaidaFlowApp.html -> 200" ($r.status -match ' 200') $r.status
    Test-Ws "http://127.0.0.1:$($dv['mirror.publicPort'])/mirror" "http://127.0.0.1:$($dv['http.port'])" 'A.'
}
Check 'A. state file names the log folder <data>\logs (config.json log.dir relative to dataDir)' ($state -and [string]$state.logDir -eq (Join-Path $DataDir 'logs')) $(if ($state) { [string]$state.logDir } else { '' })
Test-LogFolder (Join-Path $DataDir 'logs') 'A.' -Launcher
$rc = Invoke-Cmd (Stop-Cmd $Package ('-DataDir "' + $DataDir + '"')) (Join-Path $Evidence 'A-stop.console.txt')
Check 'A. stop exit code 0' ("$rc" -eq '0') "exit $rc"
$llA = Join-Path $DataDir ("logs\launcher-" + (Get-Date).ToString('yyyy-MM-dd') + '.log')
Check 'A. stop wrote to launcher-<today>.log' ((Test-Path -LiteralPath $llA) -and ((Get-Content -LiteralPath $llA) -join "`n") -match '\[stop\]\s+stopped \(exit 0\)') ''
Wait-NoListeners $allPorts 'A.'

# --- B. configured site, nginx on --------------------------------------------------------------------------
Note "--- B. config.json edited (dataDir only), install-nginx-config.ps1, start without options"
$text = [System.IO.File]::ReadAllText($pkgConfig)
$jsonDataDir = ($DataDir -replace '\\', '\\')
$text = [regex]::Replace($text, '"dataDir": "[^"]*"', ('"dataDir": "' + $jsonDataDir.Replace('$', '$$') + '"'))
[System.IO.File]::WriteAllText($pkgConfig, $text, (New-Object System.Text.UTF8Encoding($false)))
Copy-Item -LiteralPath $pkgConfig -Destination (Join-Path $Evidence 'B-edited-config.json') -Force
$site = Get-TaidaFlowConfig -Path $pkgConfig -Exe $pkgExe
Check 'B. edited config.json: dataDir = the test data folder, no warning' ($site.DataDir -eq $DataDir -and -not $site.Error -and -not @($site.Notes | Where-Object { $_ -match 'WARNING' }).Count) "dataDir $($site.DataDir)"
$sv = $site.Values
if ($SeedDb -ne "") {
    New-Item -ItemType Directory -Force (Join-Path $DataDir 'data') | Out-Null
    Copy-Item -LiteralPath (Full $SeedDb) -Destination (Join-Path $DataDir 'data')
    Note "TEST FIXTURE: copied $SeedDb -> $DataDir\data (history rows for the export check)"
}
$installOut = Join-Path $Evidence 'B-install-nginx-config.console.txt'
$rc = Invoke-Cmd ('"' + $ps + '" -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $Package 'scripts\install-nginx-config.ps1') + '"') $installOut
$installText = (Get-Content -LiteralPath $installOut -ErrorAction SilentlyContinue) -join "`n"
Check 'B. install-nginx-config.ps1 exit 0, nginx -t "test is successful"' ("$rc" -eq '0' -and $installText -match 'test is successful') "exit $rc"
$nconf = Join-Path $Package 'nginx\conf\nginx.conf'
if (Test-Path $nconf) {
    Copy-Item -LiteralPath $nconf -Destination (Join-Path $Evidence 'B-nginx.conf') -Force
    $ctext = [System.IO.File]::ReadAllText($nconf)
    Check 'B. nginx.conf: TaidaFlow header with generator, config.json path + SHA-256, install folder' ($ctext.StartsWith('# TAIDAFLOW NGINX CONFIGURATION') -and $ctext -match '# generator\s+:' -and $ctext.Contains("# config.json    : $pkgConfig") -and $ctext.Contains((Get-Sha $pkgConfig)) -and $ctext.Contains("# install folder : $Package")) ''
    Check 'B. nginx.conf: web root relative to the nginx folder (root "../web")' ($ctext -match 'root\s+"\.\./web";') ''
    Check "B. nginx.conf: listen $($sv['nginx.port']), /api/ -> $($sv['rest.port']), /mirror -> $($sv['mirror.internalPort']), exports $DataDir\exports" ($ctext -match "listen\s+0\.0\.0\.0:$($sv['nginx.port']);" -and $ctext -match "proxy_pass\s+http://127\.0\.0\.1:$($sv['rest.port']);" -and $ctext -match "proxy_pass\s+http://127\.0\.0\.1:$($sv['mirror.internalPort'])/mirror;" -and $ctext.Contains((($DataDir -replace '\\', '/') + '/exports'))) ''
}
$rc = Invoke-Cmd (Start-Cmd $Package '') (Join-Path $Evidence 'B-start.console.txt')
Check 'B. start exit code 0 (nginx + app)' ("$rc" -eq '0') "exit $rc"
$startText = (Get-Content -LiteralPath (Join-Path $Evidence 'B-start.console.txt') -ErrorAction SilentlyContinue) -join "`n"
Check "B. start script: nginx.conf up to date, nginx started like 'start nginx'" ($startText -match 'NGINX_CONF=unchanged' -and $startText -match "starting nginx like") ''
$state = Get-State
$app = if ($state) { Get-Process -Id ([int]$state.pid) -ErrorAction SilentlyContinue } else { $null }
Check 'B. TaidaFlowApp running (state file pid)' ([bool]$app) $(if ($state) { "pid $($state.pid)" } else { 'no state file' })
$ngx = Get-NginxOf $Package
Check 'B. bundled nginx running (nginx\nginx.exe of the package)' ($ngx.Count -gt 0) (($ngx | ForEach-Object { $_.Id }) -join ', ')
$nginxPort = [int]$sv['nginx.port']; $httpPort = [int]$sv['http.port']; $relayPort = [int]$sv['mirror.publicPort']
if ($app) {
    Test-AppPorts $sv $app 'B.'
    $nl = @(Get-NetTCPConnection -State Listen -LocalPort $nginxPort -ErrorAction SilentlyContinue | Where-Object { @($ngx | ForEach-Object { $_.Id }) -contains $_.OwningProcess })
    Check "B. nginx listens on $nginxPort" ($nl.Count -gt 0) ''
    Start-Sleep -Seconds 3
    $mods = @($app.Modules | ForEach-Object { $_.FileName })
    $qtOutside = @($mods | Where-Object { (Split-Path -Leaf $_) -match '^(Qt6|vcruntime|msvcp1|concrt)' -and -not $_.StartsWith($Package + '\', [System.StringComparison]::OrdinalIgnoreCase) })
    [System.IO.File]::WriteAllLines((Join-Path $Evidence 'B-loaded-modules.txt'), [string[]]$mods)
    Check 'B. Qt / MSVC runtime DLLs all loaded from the package' ($qtOutside.Count -eq 0) ("{0} module(s), outside: {1}" -f $mods.Count, ($qtOutside -join ', '))
    $expB = Get-TaidaFlowRuntimeJson (Get-TaidaFlowPagePort $site)
    $webRuntime = Join-Path $Package 'web\runtime.json'
    Check "B. web\runtime.json = $expB (nginx.enabled -> nginx.port)" ((Test-Path $webRuntime) -and [System.IO.File]::ReadAllText($webRuntime) -ceq $expB) $(if (Test-Path $webRuntime) { [System.IO.File]::ReadAllText($webRuntime) } else { 'missing' })
    $lan = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' -and $_.PrefixOrigin -ne 'WellKnown' } |
             Sort-Object { if ($_.InterfaceAlias -match 'VMware|vEthernet|Virtual') { 1 } else { 0 } } | Select-Object -First 1 | ForEach-Object { $_.IPAddress })
    $hosts = @('127.0.0.1') + $lan
    Note "hosts checked: $($hosts -join ', ')"
    foreach ($h in $hosts) {
        $base = if ($nginxPort -eq 80) { "http://$h" } else { "http://${h}:$nginxPort" }
        $r = HeadInfo "$base/" @()
        Check "B. $base/ -> 302 /TaidaFlowApp.html" ($r.status -match ' 302' -and (Hdr $r 'Location') -eq '/TaidaFlowApp.html') "$($r.status), Location: $(Hdr $r 'Location')"
        $r = HeadInfo "$base/TaidaFlowApp.html" @()
        Check "B. $base/TaidaFlowApp.html -> 200 text/html (nginx, relative root ../web)" ($r.status -match ' 200' -and (Hdr $r 'Content-Type') -match 'text/html') "$($r.status), $(Hdr $r 'Content-Type'), COOP $(Hdr $r 'Cross-Origin-Opener-Policy'), COEP $(Hdr $r 'Cross-Origin-Embedder-Policy')"
        $r = HeadInfo "$base/TaidaFlowApp.wasm" @('-H', 'Accept-Encoding: gzip')
        Check "B. $base/TaidaFlowApp.wasm (gzip) -> 200 application/wasm, Content-Encoding gzip" ($r.status -match ' 200' -and (Hdr $r 'Content-Encoding') -eq 'gzip' -and (Hdr $r 'Content-Type') -eq 'application/wasm') "$($r.status), Content-Length $(Hdr $r 'Content-Length')"
        Test-Runtime "$base/runtime.json" $expB 'B. (nginx)'
        Test-Runtime "http://${h}:$httpPort/runtime.json" $expB 'B. (app fallback)'
        Test-Ws "$base/mirror" $base 'B. (nginx /mirror)'
        Test-Ws "http://${h}:$relayPort/mirror" $base 'B. (fallback relay)'
        $api = Invoke-Curl @('-D', '-', '--max-time', '20', "$base/api/")
        $apiStatus = ($api.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
        Check "B. $base/api/ (REST status via nginx) -> 200" ("$apiStatus" -match ' 200' -and (($api.out -join "`n") -match '"status"')) "$apiStatus $(($api.out | Select-Object -Last 1))"
        $api = Invoke-Curl @('-D', '-', '--max-time', '20', "$base/api/settings/frequency")
        $apiStatus = ($api.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
        $acao = ($api.out | Where-Object { $_ -match '^Access-Control-Allow-Origin:' } | Select-Object -Last 1)
        Check "B. $base/api/settings/frequency -> 200 read_frequency, CORS" ("$apiStatus" -match ' 200' -and (($api.out -join "`n") -match '"read_frequency"') -and "$acao" -match '\*') "$apiStatus $acao"
        $pre2 = Invoke-Curl @('-X', 'OPTIONS', '-D', '-', '-o', 'NUL', '--max-time', '20', '-H', 'Origin: http://example.invalid', '-H', 'Access-Control-Request-Method: PUT', '-H', 'Access-Control-Request-Headers: Content-Type', "$base/api/settings/frequency")
        $preStatus = ($pre2.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
        $preMethods = ($pre2.out | Where-Object { $_ -match '^Access-Control-Allow-Methods:' } | Select-Object -Last 1)
        Check "B. OPTIONS $base/api/settings/frequency -> 200, Allow-Methods has PUT" ("$preStatus" -match ' 200' -and "$preMethods" -match 'PUT') "$preStatus $preMethods"
        if ($h -ne '127.0.0.1') {
            foreach ($closed in @([int]$sv['rest.port'], [int]$sv['mirror.internalPort'])) {
                $direct = Invoke-Curl @('-o', 'NUL', '-w', '%{http_code}', '--connect-timeout', '3', '--max-time', '5', "http://${h}:$closed/")
                Check "B. loopback-only port refused on the LAN address (http://${h}:$closed/)" ($direct.rc -ne 0) "curl exit $($direct.rc)"
            }
        }
    }
    # export through nginx /mirror + download links
    $client = Full $MirrorClient
    if (Test-Path $client) {
        if ($ToMs -le 0) { $ToMs = [double][DateTimeOffset]::Now.ToUnixTimeMilliseconds() }
        if ($FromMs -le 0) { $FromMs = $ToMs - 3600 * 1000 }
        $savedPath = $env:PATH
        $qtRoot = if ($env:TAIDAFLOW_QT_ROOT) { $env:TAIDAFLOW_QT_ROOT } else { 'C:\Qt\6.8.3' }
        $env:PATH = (Join-Path $qtRoot 'msvc2022_64\bin') + ';' + $savedPath      # the dev-only client uses the Qt install
        $ErrorActionPreference = 'Continue'
        $wsBase = if ($nginxPort -eq 80) { 'ws://127.0.0.1' } else { "ws://127.0.0.1:$nginxPort" }
        $mc = & $client --url "$wsBase/mirror" --origin "http://127.0.0.1" --session "w2062verify" --from-ms "$FromMs" --to-ms "$ToMs" --timeout-sec 90 2>&1 | ForEach-Object { "$_" }
        $mcRc = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        $env:PATH = $savedPath
        [System.IO.File]::WriteAllLines((Join-Path $Evidence 'B-mirror-export.txt'), [string[]]$mc)
        $result = $mc | Where-Object { $_ -match 'RESULT ' } | Select-Object -Last 1
        Check "B. history export through nginx /mirror done, downloadPort = $nginxPort" ($mcRc -eq 0 -and "$result" -match "downloadPort=$nginxPort\b") "$result"
        if ("$result" -match 'fileName=(\S+)') {
            $file = $Matches[1]
            $url = if ("$result" -match ' url=(\S+)') { $Matches[1] } else { "/exports/$file" }
            $disk = Join-Path $DataDir "exports\$file"
            $diskHash = Get-Sha $disk
            $diskLen = (Get-Item -LiteralPath $disk).Length
            $qml = [System.IO.File]::ReadAllText((Join-Path $root 'TaidaFlowContent\HistoryPage.qml'))
            $fn = [regex]::Match($qml, '(?s)function exportDownloadUrl\(entry\) \{.*?\n    \}').Value
            $node = 'C:\tools\emsdk\node\16.20.0_64bit\bin\node.exe'
            foreach ($h in $hosts) {
                $js = "var Td = { pageHost: '$h' };`n$fn`nconsole.log(exportDownloadUrl({ url: '$url', downloadPort: $nginxPort }));"
                $jsFile = Join-Path $Evidence 'exportDownloadUrl.js'
                [System.IO.File]::WriteAllText($jsFile, $js)
                $link = if ((Test-Path $node) -and $fn) { (& $node $jsFile | Select-Object -First 1) } else { '' }
                Check "B. web page link for host $h" ("$link" -eq "http://${h}:$nginxPort$url") "$link"
                if ($link) {
                    $tmp = Join-Path $Evidence "download-$($h -replace '\.', '_').csv"
                    $d = Invoke-Curl @('-o', $tmp, '-D', '-', '--max-time', '60', $link)
                    $st = ($d.out | Where-Object { $_ -match '^HTTP/' } | Select-Object -Last 1)
                    $ok = (Test-Path $tmp) -and ((Get-Sha $tmp) -eq $diskHash)
                    Check "B. GET $link -> 200, same SHA-256 as the file" ("$st" -match ' 200' -and $ok) "$st"
                    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
                    $rr = HeadInfo $link @('-H', 'Range: bytes=0-99')
                    Check "B. Range bytes=0-99 on $link -> 206" ($rr.status -match ' 206' -and (Hdr $rr 'Content-Range') -eq "bytes 0-99/$diskLen") "$($rr.status), Content-Range $(Hdr $rr 'Content-Range')"
                }
            }
            $rApp = HeadInfo "http://127.0.0.1:$httpPort$url" @()
            Check "B. fallback http://127.0.0.1:$httpPort$url -> 200" ($rApp.status -match ' 200') $rApp.status
        }
    } else { Note "  (no mirror client at $client - export check skipped)" }
    # w2-065: nginx logs in the log folder (config.json log.dir), access log named by date
    $bLogDir = Join-Path $DataDir 'logs'
    $acc = Join-Path $bLogDir ("nginx-access-" + (Get-Date).ToString('yyyy-MM-dd') + '.log')
    Start-Sleep -Seconds 1
    $accText = if (Test-Path -LiteralPath $acc) { @(Get-Content -LiteralPath $acc) } else { @() }
    Check "B. nginx access log $(Split-Path -Leaf $acc) in the log folder with the requests of this check" (@($accText | Where-Object { $_ -match '"(GET|HEAD) /TaidaFlowApp\.html HTTP/1\.1" 200 ' }).Count -gt 0 -and @($accText | Where-Object { $_ -match '"GET /runtime\.json HTTP/1\.1" 200 ' }).Count -gt 0) "$($accText.Count) line(s)"
    if (Test-Path -LiteralPath $acc) { Copy-Item -LiteralPath $acc -Destination (Join-Path $Evidence 'B-nginx-access.log') -Force }
    Check 'B. nginx-error.log in the log folder (fixed name)' (Test-Path -LiteralPath (Join-Path $bLogDir 'nginx-error.log') -PathType Leaf) ''
    $ngxAccess = @(Get-ChildItem -LiteralPath (Join-Path $Package 'nginx\logs') -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^access' })
    Check 'B. no access log in nginx\logs (only nginx.pid / its start-up messages stay there)' ($ngxAccess.Count -eq 0) (($ngxAccess | ForEach-Object { $_.Name }) -join ', ')
    Check 'B. state file: app log = <data>\logs\taidaflow-<today>-full.log' ([string]$state.appLog -eq (Join-Path $bLogDir ("taidaflow-" + (Get-Date).ToString('yyyy-MM-dd') + '-full.log'))) ([string]$state.appLog)
    Test-LogFolder $bLogDir 'B.' -Launcher
    $appLog = [string]$state.appLog
    $logText = if (Test-Path $appLog) { Get-Content -LiteralPath $appLog } else { @() }
    Copy-Item -LiteralPath $appLog -Destination (Join-Path $Evidence 'B-app.log') -ErrorAction SilentlyContinue
    $bad = @($logText | Where-Object { $_ -match '(?i)module "[^"]+" is not installed|is not a type|Cannot load library|could not find the Qt platform plugin|Failed to load|plugin .* could not be loaded|QQmlApplicationEngine failed' })
    Check 'B. app log: no QML module / plugin / library load error' ($bad.Count -eq 0) ($bad -join ' | ')
}
$listing = @(Get-ChildItem -LiteralPath $DataDir -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
    "{0}{1}" -f $_.FullName.Substring($DataDir.Length + 1), $(if ($_.PSIsContainer) { '\' } else { " ($($_.Length) bytes)" }) })
[System.IO.File]::WriteAllLines((Join-Path $Evidence 'datadir-listing.txt'), [string[]]$listing)
foreach ($want in 'TaidaFlowSettings.ini', 'settings.sqlite', 'data', 'exports', 'logs') { Check "B. data folder has $want" (Test-Path (Join-Path $DataDir $want)) '' }
$launcher = Join-Path $DataDir 'logs\launcher.log'
if (Test-Path $launcher) { Copy-Item -LiteralPath $launcher -Destination (Join-Path $Evidence 'launcher.log') -Force }
$rc = Invoke-Cmd (Stop-Cmd $Package '') (Join-Path $Evidence 'B-stop.console.txt')
Check 'B. stop exit code 0' ("$rc" -eq '0') "exit $rc"
Check 'B. stop leaves nginx running (independent of the app)' ((Get-NginxOf $Package).Count -gt 0) ''
Stop-NginxOf $Package 'B.'
Wait-NoListeners $allPorts 'B.'

# --- C. moved installation (A9) ------------------------------------------------------------------------------
if ($Mode -eq 'ps1') {
    Note "--- C. whole installation copied to another folder: start must regenerate nginx.conf"
    $movedRoot = Full 'build\verify-release-moved'
    $moved = Join-Path $movedRoot (Split-Path -Leaf $Package)
    if (Test-Path $movedRoot) { Remove-Item -LiteralPath $movedRoot -Recurse -Force }
    New-Item -ItemType Directory -Force $movedRoot | Out-Null
    Copy-Item -LiteralPath $Package -Destination $movedRoot -Recurse
    $script:running = $moved
    $movedConf = Join-Path $moved 'nginx\conf\nginx.conf'
    Check 'C. copied nginx.conf still names the old install folder' ((Test-Path $movedConf) -and [System.IO.File]::ReadAllText($movedConf).Contains("# install folder : $Package")) ''
    $rc = Invoke-Cmd (Start-Cmd $moved '') (Join-Path $Evidence 'C-start.console.txt')
    $cText = (Get-Content -LiteralPath (Join-Path $Evidence 'C-start.console.txt') -ErrorAction SilentlyContinue) -join "`n"
    Check 'C. start exit code 0' ("$rc" -eq '0') "exit $rc"
    Check 'C. start detected the mismatch and regenerated nginx.conf (old one kept as .prev-<time>), nginx -t passed' ($cText -match 'NGINX_CONF=written' -and $cText -match 'kept as .*nginx\.conf\.prev-' -and $cText -match 'test is successful') ''
    if (Test-Path $movedConf) {
        $mText = [System.IO.File]::ReadAllText($movedConf)
        Copy-Item -LiteralPath $movedConf -Destination (Join-Path $Evidence 'C-nginx.conf') -Force
        Check 'C. regenerated nginx.conf names the new install folder and its config.json' ($mText.Contains("# install folder : $moved") -and $mText.Contains("# config.json    : $(Join-Path $moved 'config.json')")) ''
    }
    Check 'C. nginx of the moved folder running' ((Get-NginxOf $moved).Count -gt 0) ''
    $base = if ($nginxPort -eq 80) { 'http://127.0.0.1' } else { "http://127.0.0.1:$nginxPort" }
    $r = HeadInfo "$base/TaidaFlowApp.html" @()
    Check "C. $base/TaidaFlowApp.html -> 200 from the moved folder" ($r.status -match ' 200') $r.status
    Test-Runtime "$base/runtime.json" (Get-TaidaFlowRuntimeJson $nginxPort) 'C.'
    Test-Ws "$base/mirror" $base 'C. (nginx /mirror)'
    $rc = Invoke-Cmd (Stop-Cmd $moved '') (Join-Path $Evidence 'C-stop.console.txt')
    Check 'C. stop exit code 0' ("$rc" -eq '0') "exit $rc"
    Stop-NginxOf $moved 'C.'
    Wait-NoListeners $allPorts 'C.'
    Remove-Item -LiteralPath $movedRoot -Recurse -Force
    Note "moved copy removed: $movedRoot"
    $script:running = $Package
}

# --- D. config.json that is not valid JSON ---------------------------------------------------------------------
Note "--- D. broken config.json"
$good = [System.IO.File]::ReadAllText($pkgConfig)
[System.IO.File]::WriteAllText($pkgConfig, $good.Replace('"port": 8124', '"port": 8124,,'), (New-Object System.Text.UTF8Encoding($false)))
$shaBroken = Get-Sha $pkgConfig
$rc = Invoke-Cmd (Start-Cmd $Package '') (Join-Path $Evidence 'D-start.console.txt')
Check 'D. broken config.json: start script exit 2, nothing started' ("$rc" -eq '2' -and @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue | Where-Object { -not ($pre | Where-Object Id -eq $_.Id) }).Count -eq 0) "exit $rc"
Check 'D. broken config.json unchanged after the start script' ((Get-Sha $pkgConfig) -eq $shaBroken) ''
$saved = $env:PATH
$env:PATH = $cleanPath
$env:QT_QPA_PLATFORM = 'offscreen'
# the package ships only platforms\qwindows.dll: for this dev-only check (no error dialog on the desktop) the
# offscreen plugin comes from the Qt installation; the app's own DLLs still come from the package
$qtRootD = if ($env:TAIDAFLOW_QT_ROOT) { $env:TAIDAFLOW_QT_ROOT } else { 'C:\Qt\6.8.3' }
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRootD 'msvc2022_64\plugins\platforms'
$env:TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS = '1500'
Remove-Item Env:\TAIDAFLOW_CONFIG -ErrorAction SilentlyContinue
$appErr = Join-Path $Evidence 'D-app.stderr.txt'
try {
    $ap = Start-Process -FilePath $pkgExe -WorkingDirectory $Evidence -PassThru -RedirectStandardError $appErr -RedirectStandardOutput "$appErr.stdout"
    $null = $ap.Handle
    $listenWhile = @()
    if (-not $ap.WaitForExit(500)) { $listenWhile = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $ap.Id }) }
    $exited = $ap.WaitForExit(30000)
    if (-not $exited) { $null = $ap.CloseMainWindow(); $null = $ap.WaitForExit(10000) }
} finally {
    $env:PATH = $saved
    Remove-Item Env:\QT_QPA_PLATFORM, Env:\QT_QPA_PLATFORM_PLUGIN_PATH, Env:\TAIDAFLOW_CONFIG_ERROR_DIALOG_TIMEOUT_MS -ErrorAction SilentlyContinue
}
$appText = if (Test-Path $appErr) { (Get-Content -LiteralPath $appErr) -join ' | ' } else { '' }
Check 'D. broken config.json: TaidaFlowApp.exe exit 2, no listener' ($exited -and $ap.ExitCode -eq 2 -and $listenWhile.Count -eq 0) "exit $($ap.ExitCode), listeners $($listenWhile.Count)"
Check 'D. broken config.json unchanged after the app (not overwritten with defaults)' ((Get-Sha $pkgConfig) -eq $shaBroken) ''
Check 'D. app log names the JSON error with line/column' ($appText -match 'JSON syntax error' -and $appText -match 'line \d+, column \d+') ''
# w2-065 (w2-064 A2): the failure is also in the fallback log folder beside config.json.
$fbDir = Join-Path $Package 'logs'
$fbFull = Join-Path $fbDir ("taidaflow-" + (Get-Date).ToString('yyyy-MM-dd') + '-full.log')
$fbQuiet = Join-Path $fbDir ("taidaflow-" + (Get-Date).ToString('yyyy-MM-dd') + '.log')
$fbText = if (Test-Path -LiteralPath $fbFull) { (Get-Content -LiteralPath $fbFull -Encoding UTF8) -join "`n" } else { '' }
$fbQText = if (Test-Path -LiteralPath $fbQuiet) { (Get-Content -LiteralPath $fbQuiet -Encoding UTF8) -join "`n" } else { '' }
Check 'D. fallback log <package>\logs\taidaflow-<today>-full.log: JSON error with line/column and the exit-code-2 line' ($fbText -match 'JSON syntax error' -and $fbText -match '\[critical\] \[Config\] config file .* cannot be used: .*\(line \d+, column \d+\) - the program exits with code 2') $fbFull
Check 'D. fallback quiet log taidaflow-<today>.log has the critical line, no [info]' ($fbQText -match '\[critical\] \[Config\]' -and $fbQText -notmatch '\[info\]') $fbQuiet
foreach ($x in $fbFull, $fbQuiet) { if (Test-Path -LiteralPath $x) { Copy-Item -LiteralPath $x -Destination (Join-Path $Evidence ('D-fallback-' + (Split-Path -Leaf $x))) -Force } }

# --- E. leave the package as shipped ---------------------------------------------------------------------------
$script:running = $null
Remove-Item -LiteralPath $pkgConfig -Force
foreach ($x in @(Get-ChildItem -LiteralPath (Join-Path $Package 'nginx\conf') -File | Where-Object { $_.Name -like 'nginx.conf*' })) { Remove-Item -LiteralPath $x.FullName -Force }
foreach ($d in 'nginx\logs', 'nginx\temp', 'logs') { $p = Join-Path $Package $d; if (Test-Path $p) { Remove-Item -LiteralPath $p -Recurse -Force } }
$webRt = Join-Path $Package 'web\runtime.json'
[System.IO.File]::WriteAllText($webRt, (Get-TaidaFlowRuntimeJson (Get-TaidaFlowPagePort $defaults)), (New-Object System.Text.UTF8Encoding($false)))
Note "package restored: config.json, nginx\conf\nginx.conf*, nginx\logs, nginx\temp, logs\ (fallback log of D) removed; web\runtime.json = the default one"
Check 'E. package without config.json / nginx.conf / logs\ again' (-not (Test-Path $pkgConfig) -and -not (Test-Path (Join-Path $Package 'nginx\conf\nginx.conf')) -and -not (Test-Path (Join-Path $Package 'logs'))) ''
Test-NoMachinePaths $Package 'E.'
$leftProc = @(Get-Process TaidaFlowApp, nginx -ErrorAction SilentlyContinue | Where-Object { -not ($pre | Where-Object Id -eq $_.Id) })
Check 'E. no TaidaFlowApp / nginx of this check left' ($leftProc.Count -eq 0) (($leftProc | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')
Note ("=== {0} check(s) failed" -f $script:fails)
Save
if ($script:fails -gt 0) { exit 1 }
exit 0

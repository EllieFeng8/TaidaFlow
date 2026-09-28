# nginx web front end for TaidaFlow (w2-050, w2-062): start / stop / reload / test / status.
# DEVELOPMENT PC ONLY (Mango A6): runs nginx with its own isolated prefix (build\nginx: rendered
# conf\, logs\, temp\), so the development PC never touches the conf folder of an installed nginx.
# The plant uses the standard way instead: scripts\install-nginx-config.ps1 once (writes
# <nginx folder>\conf\nginx.conf), then "cd <nginx folder>" + "start nginx" (start-taidaflow.ps1 does
# the same when nginx is not running); "nginx -s reload" / "nginx -s quit" from that folder.
#
#   http://<host>/                         -> 302 /TaidaFlowApp.html
#   http://<host>/TaidaFlowApp.html        web page served by nginx from the deployed web folder
#   http://<host>/runtime.json             Mirror port for the page (static file, no-store; w2-062)
#   ws://<host>/mirror                     live synchronisation of the page, proxied to the app's
#                                          Mirror server 127.0.0.1:<mirror.internalPort> (w2-062)
#   http://<host>/exports/<file>           history CSV files sent by nginx straight from the app's
#                                          export folder (HTTP Range / resume supported)
#   http://<host>/api/...                  REST API (w2-060), proxied to the app's RESTManager on
#                                          127.0.0.1:<rest.port> (loopback only)
# w2-062: every value comes from config.json (scripts\taidaflow-config.ps1) - the SAME file the app
# uses: nginx.port (listen port), rest.port (/api/), mirror.internalPort (/mirror), <dataDir>\exports
# (downloads), nginx.exe (the program), log.dir (w2-065: nginx-error.log and nginx-access-YYYY-MM-DD.log in the
# log folder of config.json, beside the app's own log files). Config file: -Config, else TAIDAFLOW_CONFIG, else
# deploy\dev\config.dev.json (repository) or <folder of this package>\config.json (package).
# The command-line options below override single values once. On start / reload / test the web
# folder's runtime.json is (re)written: {"mirrorPublicPort": <nginx listen port>, "version": 1}.
# The desktop app keeps serving the page and /exports itself on http.port (8124, fallback, no Range).
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\nginx-web.ps1 -Action <start|stop|reload|test|status>
#            [-Config <config.json>] [-WebRoot <folder>] [-ExeDir <folder>] [-ExportDir <folder>]
#            [-RuntimeDir <folder>] [-Nginx <nginx.exe or folder>] [-Port <n>] [-RestPort <n>]
#            [-MirrorPort <n>] [-TimeoutSec 20]
#   (scripts\nginx-start.ps1 / scripts\nginx-stop.ps1 are shortcuts for -Action start / stop.)
#   -WebRoot    : web page folder (default <exe folder>\web, filled by scripts\deploy-web.ps1)
#   -ExeDir     : folder of TaidaFlowApp.exe (default: this package folder when it holds the exe,
#                 else build\desktop); used for the default web root and for the app's default
#                 values of keys missing in config.json
#   -ExportDir  : export folder (default <config.json dataDir>\exports; created when missing)
#   -RuntimeDir : nginx runtime folder = nginx prefix (-p): conf\ (rendered config), logs\ (nginx.pid and
#                 nginx's own start-up messages), temp\ and the state file taidaflow-nginx.json (default
#                 build\nginx, git-ignored). The request / error logs go to config.json log.dir (w2-065).
#   -Port / -RestPort / -MirrorPort / -Nginx : one-time overrides of nginx.port / rest.port /
#                 mirror.internalPort / nginx.exe. stop / status use the running instance (state file).
#
# The configuration is deploy\nginx\taidaflow.conf (template). It is rendered into
# <runtime>\conf\taidaflow.conf with the web root, the export folder, the port, the REST port and the
# Mirror port filled in, checked with nginx -t, and run as
#   nginx -p <runtime>/ -c <runtime>/conf/taidaflow.conf
#
# Rules:
#   * start / reload refuse when the web root or the export folder is or holds a symbolic link /
#     junction (reparse point): nginx for Windows has no disable_symlinks and would follow it (exit 5);
#   * start refuses when anything already listens on the port - the owner is listed (pid 4 "System"
#     = Windows HTTP.sys: IIS, WinRM, WebDAV, a URL reservation ...) and never stopped (exit 4);
#   * nginx is started only when nginx -t passes (exit 3 otherwise);
#   * stop only acts on the nginx started by this script: the master pid in the state file must be
#     alive, have the same image path and start time, and match <runtime>\logs\nginx.pid. It is
#     stopped with "nginx -s quit" (graceful); only if that instance does not exit in time it gets
#     "nginx -s stop". Any other nginx on this machine is never touched.
#   * nginx for Windows is not a Windows service; nothing is registered, no firewall or network
#     setting is changed.
#
# Exit codes: 0 = done (status: our nginx is running); 1 = no nginx started by this script is
# running (stop/reload/status); 2 = nginx.exe / web root / TaidaFlowApp.html / template / config.json
# missing or unusable; 3 = nginx -t failed; 4 = the port already in use (or already running);
# 5 = symbolic link / junction in the web root or export folder; 6 = started but not listening on the
# port in time (stopped again); 7 = stop failed (still running, or pid file does not match).
param(
    [ValidateSet('start', 'stop', 'reload', 'test', 'status')]
    [string]$Action = 'status',
    [string]$Config = "",
    [string]$WebRoot = "",
    [string]$ExeDir = "",
    [string]$ExportDir = "",
    [string]$RuntimeDir = "",
    [string]$Nginx = "",
    [ValidateRange(1, 65535)]
    [int]$Port = 80,
    [ValidateRange(1, 65535)]
    [int]$RestPort = 18080,
    [ValidateRange(1, 65535)]
    [int]$MirrorPort = 18125,
    [int]$TimeoutSec = 20
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$template = Join-Path $root 'deploy\nginx\taidaflow.conf'
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
# $port (= -Port; PowerShell variable names are case-insensitive) is the listen port.
$portGiven = $PSBoundParameters.ContainsKey('Port')
$restPortGiven = $PSBoundParameters.ContainsKey('RestPort')
$mirrorPortGiven = $PSBoundParameters.ContainsKey('MirrorPort')
# Messages go straight to stdout, so functions return only their values.
function Say([string]$m) { [Console]::Out.WriteLine($m) }
function FullPath([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $root $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
function Fwd([string]$p) { return ($p -replace '\\', '/') }
function SamePath([string]$a, [string]$b) { return [string]::Equals($a, $b, [System.StringComparison]::OrdinalIgnoreCase) }

if ($RuntimeDir -eq "") { $RuntimeDir = 'build\nginx' }
$RuntimeDir = FullPath $RuntimeDir
$stateFile = Join-Path $RuntimeDir 'taidaflow-nginx.json'
$confFile = Join-Path $RuntimeDir 'conf\taidaflow.conf'
$pidFile = Join-Path $RuntimeDir 'logs\nginx.pid'
if ($ExeDir -ne "") { $ExeDir = FullPath $ExeDir }
elseif (Test-Path (Join-Path $root 'TaidaFlowApp.exe') -PathType Leaf) { $ExeDir = $root }
else { $ExeDir = FullPath 'build\desktop' }

# config.json -> $script:cfg (only the actions that render a configuration need it).
$script:cfg = $null
function Read-Config {
    $path = Resolve-TaidaFlowConfigPath $root $Config
    $c = Get-TaidaFlowConfig -Path $path -Exe (Join-Path $ExeDir 'TaidaFlowApp.exe')
    Say "config.json: $path$(if (-not $c.Exists) { ' (does not exist - app defaults)' })"
    foreach ($n in $c.Notes) { Say "  config note: $n" }
    if ($c.Error) { Say "config.json unusable: $($c.Error)"; exit 2 }
    $script:cfg = $c
    if (-not $portGiven) { $script:port = [int]$c.Values['nginx.port'] }
    if (-not $restPortGiven) { $script:RestPort = [int]$c.Values['rest.port'] }
    if (-not $mirrorPortGiven) { $script:MirrorPort = [int]$c.Values['mirror.internalPort'] }
    Say ("  nginx.port {0}{1}, rest.port {2}{3}, mirror.internalPort {4}{5}, dataDir {6}" -f $script:port, $(if ($portGiven) { ' (-Port)' } else { '' }),
         $script:RestPort, $(if ($restPortGiven) { ' (-RestPort)' } else { '' }), $script:MirrorPort, $(if ($mirrorPortGiven) { ' (-MirrorPort)' } else { '' }), $c.DataDir)
    if (-not $c.Values['nginx.enabled']) { Say "  NOTE: config.json nginx.enabled is false - the app then points pages at mirror.publicPort / http.port, not at nginx" }
}

function Find-Nginx {
    if ($Nginx -ne "") { $source = '-Nginx'; $candidate = $Nginx }
    else {
        if (-not $script:cfg) { Read-Config }
        $source = "config.json nginx.exe = $($script:cfg.Values['nginx.exe']) ($($script:cfg.Sources['nginx.exe']); relative = to the config.json folder)"
        $candidate = Resolve-TaidaFlowNginxExe $script:cfg
    }
    if ($candidate -ne '' -and (Test-Path $candidate -PathType Container)) { $candidate = Join-Path $candidate 'nginx.exe' }
    if ($candidate -eq '' -or -not (Test-Path $candidate -PathType Leaf)) {
        Say "nginx.exe not found (source: $source, path: '$candidate'). Install nginx for Windows (nginx.org zip, e.g. 1.30.x) and set config.json nginx.exe to its nginx.exe, or pass -Nginx"
        exit 2
    }
    $full = [System.IO.Path]::GetFullPath($candidate)
    Say "nginx: $full (from $source)"
    return $full
}

# Runs nginx.exe for a short command (-t, -s quit, -s reload ...); returns exit code, prints output.
function Invoke-Nginx([string]$exe, [string[]]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.WorkingDirectory = $RuntimeDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $err = $errTask.Result
    foreach ($line in (($out + $err) -split "`r?`n")) { if ($line.Trim() -ne '') { Say "  | $line" } }
    return $p.ExitCode
}

# Symbolic links / junctions anywhere below (or at) the web root; links are not followed.
function Find-ReparsePoints([string]$dir) {
    $found = New-Object System.Collections.Generic.List[string]
    $top = New-Object System.IO.DirectoryInfo $dir
    if ($top.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $found.Add($top.FullName); return ,$found }
    $stack = New-Object System.Collections.Generic.Stack[System.IO.DirectoryInfo]
    $stack.Push($top)
    while ($stack.Count -gt 0) {
        $d = $stack.Pop()
        foreach ($e in $d.EnumerateFileSystemInfos()) {
            if ($e.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { $found.Add($e.FullName) }
            elseif ($e -is [System.IO.DirectoryInfo]) { $stack.Push($e) }
        }
    }
    return ,$found
}

function Resolve-WebRoot([string]$fromState) {
    if ($WebRoot -ne "") { $w = FullPath $WebRoot }
    elseif ($fromState) { $w = $fromState }
    else {
        $w = Join-Path $ExeDir 'web'
    }
    if (-not (Test-Path $w -PathType Container)) {
        Say "web root not found: $w (deploy first: scripts\deploy-web.ps1)"; exit 2
    }
    if (-not (Test-Path (Join-Path $w 'TaidaFlowApp.html') -PathType Leaf)) {
        Say "no TaidaFlowApp.html in $w (deploy first: scripts\deploy-web.ps1)"; exit 2
    }
    Assert-UsablePath 'web root' $w
    Assert-NoLinks 'web root' $w 'remove them (or deploy again with scripts\deploy-web.ps1)'
    return $w
}

# The path goes into the nginx configuration: refuse characters with a meaning there.
function Assert-UsablePath([string]$what, [string]$path) {
    if ($path -match '[\$"''{};#]' -or $path -match '[\x00-\x1f]') {
        Say "$what path contains a character that cannot be used in the nginx configuration ($ `" ' { } ; #): $path"; exit 2
    }
}

function Assert-NoLinks([string]$what, [string]$path, [string]$advice) {
    $links = Find-ReparsePoints $path
    if ($links.Count -gt 0) {
        Say "REFUSED: the $what $path is or holds $($links.Count) symbolic link(s) / junction(s); nginx for"
        Say "  Windows has no disable_symlinks and would serve files outside the folder through them:"
        $links | ForEach-Object { Say "    $_" }
        Say "  $advice - nginx NOT started / reloaded"
        exit 5
    }
}

function Resolve-ExportDir([string]$fromState) {
    if ($ExportDir -ne "") { $x = FullPath $ExportDir }
    elseif ($fromState) { $x = $fromState }
    else {
        if (-not $script:cfg) { Read-Config }
        $x = Join-Path $script:cfg.DataDir 'exports'   # the app's export folder (<dataDir>\exports)
    }
    if (-not (Test-Path $x -PathType Container)) {
        New-Item -ItemType Directory -Force $x | Out-Null
        Say "export folder created: $x (the app creates the same folder at start-up)"
    }
    Assert-UsablePath 'export folder' $x
    Assert-NoLinks 'export folder' $x 'remove them by hand (rmdir <junction> removes only the link)'
    return $x
}

function Write-Config([string]$web, [string]$exports) {
    if (-not (Test-Path $template -PathType Leaf)) { Say "template missing: $template"; exit 2 }
    foreach ($sub in 'conf', 'logs', 'temp') { New-Item -ItemType Directory -Force (Join-Path $RuntimeDir $sub) | Out-Null }
    # w2-065: nginx-error.log / nginx-access-YYYY-MM-DD.log go to config.json log.dir (nginx does not create it).
    $script:logDir = Resolve-TaidaFlowLogDir $script:cfg
    Assert-UsablePath 'log folder' $script:logDir
    New-Item -ItemType Directory -Force $script:logDir | Out-Null
    try { $body = Get-TaidaFlowNginxConf $template @{ WebRoot = $web; ExportDir = $exports; LogDir = $script:logDir; Port = $port; RestPort = $RestPort; MirrorPort = $MirrorPort; Prefix = $RuntimeDir } }
    catch { Say $_.Exception.Message; exit 2 }
    $header = "# GENERATED by scripts\nginx-web.ps1 (DEVELOPMENT, prefix $RuntimeDir) from deploy\nginx\taidaflow.conf - edit the template, not this file.`n" +
              "# web root: $web`n# export folder: $exports`n# port: $port`n# REST port (/api/ -> 127.0.0.1): $RestPort`n" +
              "# Mirror port (/mirror -> 127.0.0.1): $MirrorPort`n# log folder: $($script:logDir) (nginx-error.log, nginx-access-YYYY-MM-DD.log)`n"
    $text = $header + $body
    [System.IO.File]::WriteAllText($confFile, $text, (New-Object System.Text.UTF8Encoding($false)))
    Say "config: $confFile (web root $web, export folder $exports, log folder $($script:logDir), port $port, /api/ -> 127.0.0.1:$RestPort, /mirror -> 127.0.0.1:$MirrorPort)"
}

# runtime.json of the web root: the page connects its Mirror through this nginx (w2-062, Mango A2).
function Write-Runtime([string]$web) {
    Say ("  " + (Write-TaidaFlowRuntimeJson $web $port))
}

function Test-Config([string]$exe) {
    Say "nginx -t:"
    $rc = Invoke-Nginx $exe @('-p', ((Fwd $RuntimeDir) + '/'), '-c', (Fwd $confFile), '-t')
    if ($rc -ne 0) { Say "nginx -t FAILED (exit $rc)" }
    return $rc
}

function Get-Workers([int]$masterPid) {
    return @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$masterPid" -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -eq 'nginx.exe' } | ForEach-Object { [int]$_.ProcessId })
}

# The nginx master started by this script (state file + same image + same start time), or $null.
function Get-OurInstance {
    if (-not (Test-Path $stateFile)) { return $null }
    $s = Get-Content -Raw $stateFile | ConvertFrom-Json
    $p = Get-Process -Id ([int]$s.pid) -ErrorAction SilentlyContinue
    if (-not $p) { return $null }
    $path = try { $p.Path } catch { '' }
    $ticks = try { $p.StartTime.ToUniversalTime().Ticks } catch { 0 }
    if (-not (SamePath $path $s.exe) -or [int64]$ticks -ne [int64]$s.startTicksUtc) { return $null }
    return [pscustomobject]@{ state = $s; process = $p }
}

function Show-Listeners {
    $l = @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)
    foreach ($x in $l) {
        $pname = try { $pp = Get-Process -Id $x.OwningProcess -ErrorAction Stop; ("$($pp.ProcessName) $($pp.Path)").Trim() } catch { '?' }
        if ($x.OwningProcess -eq 4) {
            $pname = 'System = Windows HTTP.sys (kernel HTTP server used by IIS, WinRM, WebDAV, a URL reservation ...; ' +
                     'see "netsh http show servicestate" - not stopped by this script)'
        }
        Say ("  listener {0}:{1} pid={2} ({3})" -f $x.LocalAddress, $x.LocalPort, $x.OwningProcess, $pname)
    }
    return $l
}

function Show-OtherNginx([int[]]$ours) {
    $others = @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { $ours -notcontains $_.Id })
    foreach ($o in $others) {
        $path = try { $o.Path } catch { '?' }
        Say ("  other nginx (not started by this script, not touched): pid={0} {1}" -f $o.Id, $path)
    }
}

switch ($Action) {
    'test' {
        Read-Config
        $exe = Find-Nginx
        $web = Resolve-WebRoot $null
        $exports = Resolve-ExportDir $null
        Write-Config $web $exports
        $rc = Test-Config $exe
        if ($rc -ne 0) { exit 3 }
        Write-Runtime $web
        exit 0
    }
    'status' {
        $inst = Get-OurInstance
        if ($inst -and -not $portGiven -and $inst.state.port) { $port = [int]$inst.state.port }
        if ($inst) {
            $ours = @([int]$inst.state.pid) + (Get-Workers ([int]$inst.state.pid))
            Say ("running: master pid {0}, workers {1}, web root {2}, export folder {3}, runtime {4}, REST port {5}, Mirror port {6}" -f $inst.state.pid, ((Get-Workers ([int]$inst.state.pid)) -join ','), $inst.state.webRoot, $inst.state.exportDir, $RuntimeDir, $(if ($inst.state.restPort) { $inst.state.restPort } else { '(not recorded)' }), $(if ($inst.state.mirrorPort) { $inst.state.mirrorPort } else { '(not recorded)' }))
        } else {
            $ours = @()
            Say "no nginx started by this script is running (runtime $RuntimeDir)"
        }
        $null = Show-Listeners
        Show-OtherNginx $ours
        if ($inst) { exit 0 } else { exit 1 }
    }
    'start' {
        $inst = Get-OurInstance
        if ($inst) { Say "already running (master pid $($inst.state.pid)) - use -Action reload or stop"; exit 4 }
        Read-Config
        $exe = Find-Nginx
        $web = Resolve-WebRoot $null
        $exports = Resolve-ExportDir $null
        $busy = @(Show-Listeners)
        if ($busy.Count -gt 0) {
            Say "port $port is already in use - nginx NOT started (the owner is not stopped)"
            exit 4
        }
        Write-Config $web $exports
        if ((Test-Config $exe) -ne 0) { Say 'nginx NOT started'; exit 3 }
        Write-Runtime $web
        if (Test-Path $pidFile) { Remove-Item -LiteralPath $pidFile -Force }
        $logs = Join-Path $RuntimeDir 'logs'
        # No output redirection on purpose: Start-Process then uses ShellExecute, so nginx does not
        # inherit this script's handles (a redirected stdout of this script would otherwise stay
        # open until nginx exits and block the caller). nginx writes to <log.dir>\nginx-error.log; the
        # configuration was already checked with nginx -t above.
        $p = Start-Process -FilePath $exe -WorkingDirectory $RuntimeDir -WindowStyle Hidden -PassThru `
                 -ArgumentList @('-p', ('"' + (Fwd $RuntimeDir) + '/"'), '-c', ('"' + (Fwd $confFile) + '"'))
        $null = $p.Handle
        $state = [ordered]@{
            pid = $p.Id; exe = $exe; startTicksUtc = $p.StartTime.ToUniversalTime().Ticks
            startTime = $p.StartTime.ToString('o'); prefix = (Fwd $RuntimeDir) + '/'; conf = (Fwd $confFile)
            webRoot = $web; exportDir = $exports; logDir = $script:logDir; port = $port; restPort = $RestPort; mirrorPort = $MirrorPort
            config = $script:cfg.Path
        }
        [System.IO.File]::WriteAllText($stateFile, ($state | ConvertTo-Json), (New-Object System.Text.UTF8Encoding($false)))
        $t0 = Get-Date
        $listening = $false
        do {
            Start-Sleep -Milliseconds 250
            $ours = @($p.Id) + (Get-Workers $p.Id)
            $l = @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue |
                   Where-Object { $ours -contains $_.OwningProcess })
            $listening = $l.Count -gt 0
        } while (-not $listening -and -not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec)
        if (-not $listening) {
            Say "nginx did not listen on $port within $TimeoutSec s"
            if (-not $p.HasExited) {
                $null = Invoke-Nginx $exe @('-p', ((Fwd $RuntimeDir) + '/'), '-c', (Fwd $confFile), '-s', 'stop')
                $null = $p.WaitForExit(10000)
            } else { Say "nginx exited with code $($p.ExitCode)" }
            foreach ($f in (Join-Path $script:logDir 'nginx-error.log'), (Join-Path $logs 'error.log')) {
                if (Test-Path $f) { Say "  $f :"; Get-Content $f -Tail 10 | ForEach-Object { Say "  | $_" } }
            }
            Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
            exit 6
        }
        $workers = Get-Workers $p.Id
        $pidInFile = if (Test-Path $pidFile) { (Get-Content -Raw $pidFile).Trim() } else { '' }
        Say ("NGINX_PID={0} WORKERS={1} PIDFILE={2}" -f $p.Id, ($workers -join ','), $pidInFile)
        Say "WEBROOT=$web"
        Say "EXPORTDIR=$exports"
        Say "LOGDIR=$($script:logDir) (nginx-error.log, nginx-access-YYYY-MM-DD.log)"
        Say "RUNTIME=$RuntimeDir"
        Say "URL=http://<this host's IPv4>:$port/  (-> /TaidaFlowApp.html)  downloads: http://<this host's IPv4>:$port/exports/<file>"
        Say "MIRROR=ws://<this host's IPv4>:$port/mirror  -> 127.0.0.1:$MirrorPort (the app's Mirror server; runtime.json mirrorPublicPort=$port)"
        Say "REST=http://<this host's IPv4>:$port/api/...  -> 127.0.0.1:$RestPort (the app's RESTManager)"
        Say "(the app takes its download-link port and runtime.json from config.json: nginx.enabled true -> nginx.port)"
        exit 0
    }
    'reload' {
        $inst = Get-OurInstance
        if (-not $inst) { Say "no nginx started by this script is running - nothing to reload"; exit 1 }
        # w2-062: reload re-reads config.json (edit config.json, then reload); options override once.
        Read-Config
        $oldPort = if ($inst.state.port) { [int]$inst.state.port } else { 8123 }
        if ($port -ne $oldPort) {
            $busy = @(Show-Listeners)
            if ($busy.Count -gt 0) { Say "port $port is already in use - configuration NOT reloaded (the owner is not stopped)"; exit 4 }
            Say "moving nginx from port $oldPort to $port"
        }
        $exe = $inst.state.exe
        $web = Resolve-WebRoot $inst.state.webRoot
        $exports = Resolve-ExportDir $null
        Write-Config $web $exports
        if ((Test-Config $exe) -ne 0) { Say 'configuration NOT reloaded (the running one stays)'; exit 3 }
        Write-Runtime $web
        $rc = Invoke-Nginx $exe @('-p', $inst.state.prefix, '-c', $inst.state.conf, '-s', 'reload')
        if ($rc -ne 0) { Say "nginx -s reload failed (exit $rc)"; exit 3 }
        $s = $inst.state
        $s.webRoot = $web
        $s.exportDir = $exports
        if ($s.PSObject.Properties['port']) { $s.port = $port } else { $s | Add-Member -NotePropertyName port -NotePropertyValue $port }
        if ($s.PSObject.Properties['restPort']) { $s.restPort = $RestPort } else { $s | Add-Member -NotePropertyName restPort -NotePropertyValue $RestPort }
        if ($s.PSObject.Properties['mirrorPort']) { $s.mirrorPort = $MirrorPort } else { $s | Add-Member -NotePropertyName mirrorPort -NotePropertyValue $MirrorPort }
        [System.IO.File]::WriteAllText($stateFile, ($s | ConvertTo-Json), (New-Object System.Text.UTF8Encoding($false)))
        Say "reloaded (master pid $($s.pid), port $port, REST port $RestPort, Mirror port $MirrorPort, web root $web, export folder $exports)"
        exit 0
    }
    'stop' {
        $inst = Get-OurInstance
        if (-not $inst) {
            Say "no nginx started by this script is running (runtime $RuntimeDir) - nothing stopped"
            if (Test-Path $stateFile) { Remove-Item -LiteralPath $stateFile -Force; Say '  stale state file removed' }
            Show-OtherNginx @()
            exit 1
        }
        $masterPid = [int]$inst.state.pid
        if ($inst.state.port) { $port = [int]$inst.state.port }
        $exe = $inst.state.exe
        $pidInFile = if (Test-Path $pidFile) { (Get-Content -Raw $pidFile).Trim() } else { '' }
        if ($pidInFile -ne "$masterPid") {
            Say "pid file $pidFile says '$pidInFile', state file says $masterPid - not signalling (nothing stopped)"
            exit 7
        }
        $ours = @($masterPid) + (Get-Workers $masterPid)
        $procs = @($ours | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue } | Where-Object { $_ })
        $procs | ForEach-Object { $null = $_.Handle }
        Say ("stopping nginx master pid {0} (workers {1}) with nginx -s quit" -f $masterPid, (($ours | Select-Object -Skip 1) -join ','))
        $null = Invoke-Nginx $exe @('-p', $inst.state.prefix, '-c', $inst.state.conf, '-s', 'quit')
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while (@($procs | Where-Object { -not $_.HasExited }).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
        if (@($procs | Where-Object { -not $_.HasExited }).Count -gt 0) {
            Say "  not exited after $TimeoutSec s (open connections?) - nginx -s stop"
            $null = Invoke-Nginx $exe @('-p', $inst.state.prefix, '-c', $inst.state.conf, '-s', 'stop')
            $deadline = (Get-Date).AddSeconds(10)
            while (@($procs | Where-Object { -not $_.HasExited }).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
        }
        $left = @($procs | Where-Object { -not $_.HasExited })
        if ($left.Count -gt 0) {
            Say ("STOP FAILED: still running: " + (($left | ForEach-Object { $_.Id }) -join ', '))
            exit 7
        }
        Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
        $still = @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue | Where-Object { $ours -contains $_.OwningProcess })
        Say ("stopped: pids {0} exited; listeners of these pids on {1}: {2}" -f ($ours -join ','), $port, $still.Count)
        Show-OtherNginx @()
        exit 0
    }
}

# nginx web front end for TaidaFlow (w2-050): start / stop / reload / test / status.
#
#   http://<host>:8123/TaidaFlowApp.html   web page served by nginx from the deployed web folder
#   http://<host>:8123/exports/<file>      history CSV files sent by nginx straight from the app's
#                                          export folder (HTTP Range / resume supported)
# The desktop app keeps serving the page and /exports itself on :8124 (fallback, no Range) - both
# addresses work. Start the app with TAIDAFLOW_DOWNLOAD_PORT=8123 so that the download links the
# page receives point to nginx.
#
# Usage: powershell -ExecutionPolicy Bypass -File scripts\nginx-web.ps1 -Action <start|stop|reload|test|status>
#            [-WebRoot <folder>] [-ExeDir <folder>] [-ExportDir <folder>] [-RuntimeDir <folder>]
#            [-Nginx <nginx.exe or folder>] [-TimeoutSec 20]
#   (scripts\nginx-start.ps1 / scripts\nginx-stop.ps1 are shortcuts for -Action start / stop.)
#   -WebRoot    : web page folder (default <exe folder>\web, filled by scripts\deploy-web.ps1)
#   -ExeDir     : exe folder for the default web root (default build\desktop)
#   -ExportDir  : the app's export folder = <app working directory>\exports (default
#                 build\runtime-cwd\exports, the working directory of scripts\run-desktop.ps1);
#                 created when missing (the app creates the same folder at start-up)
#   -RuntimeDir : nginx runtime folder = nginx prefix (-p): conf\ (rendered config), logs\, temp\
#                 and the state file taidaflow-nginx.json (default build\nginx, git-ignored)
#   -Nginx      : nginx.exe (or its folder). Otherwise the environment variable TAIDAFLOW_NGINX,
#                 otherwise the newest C:\tools\nginx\nginx-<version>\nginx.exe.
#
# The configuration is deploy\nginx\taidaflow.conf (template, in the repository). It is rendered
# into <runtime>\conf\taidaflow.conf with the web root and the export folder filled in, checked
# with nginx -t, and run as
#   nginx -p <runtime>/ -c <runtime>/conf/taidaflow.conf
#
# Rules:
#   * start / reload refuse when the web root or the export folder is or holds a symbolic link /
#     junction (reparse point): nginx for Windows has no disable_symlinks and would follow it (exit 5);
#   * start refuses when anything already listens on port 8123 - the owner is never stopped (exit 4);
#   * nginx is started only when nginx -t passes (exit 3 otherwise);
#   * stop only acts on the nginx started by this script: the master pid in the state file must be
#     alive, have the same image path and start time, and match <runtime>\logs\nginx.pid. It is
#     stopped with "nginx -s quit" (graceful); only if that instance does not exit in time it gets
#     "nginx -s stop". Any other nginx on this machine is never touched.
#   * nginx for Windows is not a Windows service; nothing is registered, no firewall or network
#     setting is changed.
#
# Exit codes: 0 = done (status: our nginx is running); 1 = no nginx started by this script is
# running (stop/reload/status); 2 = nginx.exe / web root / TaidaFlowApp.html / template missing or
# unusable path; 3 = nginx -t failed; 4 = port 8123 already in use (or already running);
# 5 = symbolic link / junction in the web root or export folder; 6 = started but not listening on 8123 in time
# (stopped again); 7 = stop failed (still running, or pid file does not match).
param(
    [ValidateSet('start', 'stop', 'reload', 'test', 'status')]
    [string]$Action = 'status',
    [string]$WebRoot = "",
    [string]$ExeDir = "",
    [string]$ExportDir = "",
    [string]$RuntimeDir = "",
    [string]$Nginx = "",
    [int]$TimeoutSec = 20
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$template = Join-Path $root 'deploy\nginx\taidaflow.conf'
$port = 8123

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

function Find-Nginx {
    $source = ''
    $candidate = ''
    if ($Nginx -ne "") { $source = '-Nginx'; $candidate = $Nginx }
    elseif ($env:TAIDAFLOW_NGINX) { $source = 'TAIDAFLOW_NGINX'; $candidate = $env:TAIDAFLOW_NGINX }
    else {
        $source = 'C:\tools\nginx (newest version)'
        $best = $null; $bestVer = $null
        if (Test-Path 'C:\tools\nginx' -PathType Container) {
            foreach ($d in Get-ChildItem 'C:\tools\nginx' -Directory -Filter 'nginx-*') {
                $v = $null
                if ([version]::TryParse($d.Name.Substring(6), [ref]$v) -and (Test-Path (Join-Path $d.FullName 'nginx.exe'))) {
                    if ($null -eq $bestVer -or $v -gt $bestVer) { $best = $d.FullName; $bestVer = $v }
                }
            }
        }
        if ($best) { $candidate = $best }
    }
    if ($candidate -ne '' -and (Test-Path $candidate -PathType Container)) { $candidate = Join-Path $candidate 'nginx.exe' }
    if ($candidate -eq '' -or -not (Test-Path $candidate -PathType Leaf)) {
        Say "nginx.exe not found (source: $source, path: '$candidate'). Pass -Nginx, set TAIDAFLOW_NGINX or unpack the nginx.org Windows zip to C:\tools\nginx\nginx-<version>\"
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
        $e = if ($ExeDir -ne "") { FullPath $ExeDir } else { FullPath 'build\desktop' }
        $w = Join-Path $e 'web'
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
    else { $x = FullPath 'build\runtime-cwd\exports' }
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
    $text = [System.IO.File]::ReadAllText($template)
    foreach ($token in '@TAIDAFLOW_WEB_ROOT@', '@TAIDAFLOW_EXPORT_DIR@') {
        if (-not $text.Contains($token)) { Say "template has no ${token}: $template"; exit 2 }
    }
    $header = "# GENERATED by scripts\nginx-web.ps1 from deploy\nginx\taidaflow.conf - edit the template, not this file.`n" +
              "# web root: $web`n# export folder: $exports`n"
    $text = $header + $text.Replace('@TAIDAFLOW_WEB_ROOT@', (Fwd $web)).Replace('@TAIDAFLOW_EXPORT_DIR@', (Fwd $exports))
    [System.IO.File]::WriteAllText($confFile, $text, (New-Object System.Text.UTF8Encoding($false)))
    Say "config: $confFile (web root $web, export folder $exports)"
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
        $pname = try { (Get-Process -Id $x.OwningProcess -ErrorAction Stop).ProcessName } catch { '?' }
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
        $exe = Find-Nginx
        $web = Resolve-WebRoot $null
        $exports = Resolve-ExportDir $null
        Write-Config $web $exports
        $rc = Test-Config $exe
        if ($rc -ne 0) { exit 3 }
        exit 0
    }
    'status' {
        $inst = Get-OurInstance
        if ($inst) {
            $ours = @([int]$inst.state.pid) + (Get-Workers ([int]$inst.state.pid))
            Say ("running: master pid {0}, workers {1}, web root {2}, export folder {3}, runtime {4}" -f $inst.state.pid, ((Get-Workers ([int]$inst.state.pid)) -join ','), $inst.state.webRoot, $inst.state.exportDir, $RuntimeDir)
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
        if (Test-Path $pidFile) { Remove-Item -LiteralPath $pidFile -Force }
        $logs = Join-Path $RuntimeDir 'logs'
        # No output redirection on purpose: Start-Process then uses ShellExecute, so nginx does not
        # inherit this script's handles (a redirected stdout of this script would otherwise stay
        # open until nginx exits and block the caller). nginx writes to logs\error.log; the
        # configuration was already checked with nginx -t above.
        $p = Start-Process -FilePath $exe -WorkingDirectory $RuntimeDir -WindowStyle Hidden -PassThru `
                 -ArgumentList @('-p', ('"' + (Fwd $RuntimeDir) + '/"'), '-c', ('"' + (Fwd $confFile) + '"'))
        $null = $p.Handle
        $state = [ordered]@{
            pid = $p.Id; exe = $exe; startTicksUtc = $p.StartTime.ToUniversalTime().Ticks
            startTime = $p.StartTime.ToString('o'); prefix = (Fwd $RuntimeDir) + '/'; conf = (Fwd $confFile)
            webRoot = $web; exportDir = $exports; port = $port
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
            $f = Join-Path $logs 'error.log'
            if (Test-Path $f) { Get-Content $f -Tail 10 | ForEach-Object { Say "  | $_" } }
            Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
            exit 6
        }
        $workers = Get-Workers $p.Id
        $pidInFile = if (Test-Path $pidFile) { (Get-Content -Raw $pidFile).Trim() } else { '' }
        Say ("NGINX_PID={0} WORKERS={1} PIDFILE={2}" -f $p.Id, ($workers -join ','), $pidInFile)
        Say "WEBROOT=$web"
        Say "EXPORTDIR=$exports"
        Say "RUNTIME=$RuntimeDir"
        Say "URL=http://<this host's IPv4>:$port/TaidaFlowApp.html  downloads: http://<this host's IPv4>:$port/exports/<file>"
        Say "(start the desktop app with TAIDAFLOW_DOWNLOAD_PORT=$port so that its download links point here)"
        exit 0
    }
    'reload' {
        $inst = Get-OurInstance
        if (-not $inst) { Say "no nginx started by this script is running - nothing to reload"; exit 1 }
        $exe = $inst.state.exe
        $web = Resolve-WebRoot $inst.state.webRoot
        $exports = Resolve-ExportDir $inst.state.exportDir
        Write-Config $web $exports
        if ((Test-Config $exe) -ne 0) { Say 'configuration NOT reloaded (the running one stays)'; exit 3 }
        $rc = Invoke-Nginx $exe @('-p', $inst.state.prefix, '-c', $inst.state.conf, '-s', 'reload')
        if ($rc -ne 0) { Say "nginx -s reload failed (exit $rc)"; exit 3 }
        $s = $inst.state
        $s.webRoot = $web
        $s.exportDir = $exports
        [System.IO.File]::WriteAllText($stateFile, ($s | ConvertTo-Json), (New-Object System.Text.UTF8Encoding($false)))
        Say "reloaded (master pid $($s.pid), web root $web, export folder $exports)"
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

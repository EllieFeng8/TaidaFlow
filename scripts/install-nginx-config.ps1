# TaidaFlow - write the nginx configuration of the plant once (w2-062, Mango A6/A7).
#
# nginx on the plant PC is started the standard way, without any argument:
#     cd <nginx folder>          (the package bundles it: <installation folder>\nginx)
#     start nginx                (start-taidaflow.ps1 does the same when nginx is not running)
#     nginx -s reload            apply a changed nginx.conf
#     nginx -s quit              stop it (stop-taidaflow.ps1 leaves nginx running)
# This script writes the COMPLETE TaidaFlow configuration into <nginx folder>\conf\nginx.conf, from the
# template deploy\nginx\taidaflow.conf and config.json (the same file the app uses):
#     listen port          nginx.port
#     web root             <installation folder>\web            (the page, runtime.json)
#     /exports/<file>      <config.json dataDir>\exports        (CSV downloads with Range)
#     /api/                127.0.0.1:<rest.port>                (REST API)
#     /mirror              127.0.0.1:<mirror.internalPort>      (WebSocket, live synchronisation)
#     log folder           config.json log.dir resolved against dataDir (default <dataDir>\logs; w2-065):
#                          nginx-error.log and nginx-access-YYYY-MM-DD.log, beside the app's own log
#                          files; created when missing
#     nginx folder         folder of config.json nginx.exe (default nginx\nginx.exe = the bundled one;
#                          relative = to the folder of config.json)
# The web root is written relative to the nginx folder, the other paths absolute; nginx keeps
# logs\nginx.pid (and the messages of its own start-up) and temp\ in the nginx folder.
# An existing nginx.conf that was not written by this script is kept as nginx.conf.orig-<timestamp>
# (never deleted); an older TaidaFlow one is kept as nginx.conf.prev-<timestamp> (with -Build, the
# desktop build's own output, it is replaced without a backup). Then "nginx -t" checks the result (default prefix =
# the nginx folder). config.json is only read (created with the defaults when missing, like
# start-taidaflow.ps1 does), never changed.
# Run it again after config.json (ports, dataDir, log.dir, nginx.exe) or the installation folder changed, and
# after copying a new package over the installation folder; then "nginx -s reload" (or restart nginx).
#
# Checks (nothing is stopped or changed outside the nginx conf folder):
#   * the web folder and the export folder must not be or hold a symbolic link / junction (nginx for
#     Windows has no disable_symlinks and would follow it) -> exit 5;
#   * a listener on nginx.port is listed with its owner (pid 4 "System" = Windows HTTP.sys: IIS,
#     WinRM, WebDAV, a URL reservation ...). If it is this nginx, "nginx -s reload" applies the new file;
#     another program must be moved by the plant (it is never stopped here) - reported, exit 4 (the
#     file is still written).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\install-nginx-config.ps1
#            [-Config <config.json>] [-InstallDir <folder of TaidaFlowApp.exe>] [-Nginx <nginx.exe or folder>]
#            [-NginxDir <nginx folder>] [-Build]
#   -Config     : default <installation folder>\config.json (package) - in the repository the usual
#                 lookup (TAIDAFLOW_CONFIG, deploy\dev\config.dev.json)
#   -InstallDir : default the folder of this package (the parent of scripts\)
#   -Nginx      : another nginx.exe (or its folder) instead of config.json nginx.exe
#   -NginxDir   : write into this nginx folder instead of the folder of nginx.exe (<NginxDir>\conf\nginx.conf;
#                 nginx -t runs only when <NginxDir>\nginx.exe exists)
#   -Build      : (Mango A8) called by the desktop build (CMakeLists.txt, target taidaflow_nginx_conf) to
#                 write <build dir>\nginx\conf\nginx.conf: config.json is never created, a missing web
#                 folder / export folder is only noted, runtime.json and the port check are skipped;
#                 the keys it needs must be in that file (no app defaults yet): dataDir, nginx.*,
#                 rest.port, mirror.*Port and (w2-065) log.dir.
# Exit codes: 0 written and nginx -t passed (or skipped: no nginx.exe in -NginxDir); 2 missing file /
# folder, config.json unusable; 3 nginx -t failed (the file stays written - fix and run again); 4
# written and tested, but nginx.port is used by another program; 5 link / junction found (nothing written).
param(
    [string]$Config = "",
    [string]$InstallDir = "",
    [string]$Nginx = "",
    [string]$NginxDir = "",
    [switch]$Build,
    [switch]$IfChanged
)
$ErrorActionPreference = 'Stop'
# vcvars64.bat may launch Windows PowerShell in ConstrainedLanguage mode,
# where built-in modules are not auto-loaded. Get-FileHash below belongs to
# Microsoft.PowerShell.Utility, so load it explicitly before using it.
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
function Say([string]$m) { [Console]::Out.WriteLine($m) }
function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path (Get-Location).Path $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
$template = Join-Path $root 'deploy\nginx\taidaflow.conf'
if (-not (Test-Path $template -PathType Leaf)) { Say "template missing: $template"; exit 2 }
if ($InstallDir -eq "") { $InstallDir = $root } else { $InstallDir = Full $InstallDir }
$appExe = Join-Path $InstallDir 'TaidaFlowApp.exe'
if (-not $Build -and -not (Test-Path $appExe -PathType Leaf)) { Say "TaidaFlowApp.exe not found in $InstallDir (-InstallDir)"; exit 2 }
if ($Config -ne "") { $configPath = Full $Config }
elseif (Test-Path (Join-Path $root 'deploy\dev\config.dev.json')) { $configPath = Resolve-TaidaFlowConfigPath $root '' }
else { $configPath = Join-Path $InstallDir 'config.json' }

if ($Build) {
    # The desktop build runs this BEFORE TaidaFlowApp.exe is linked (the exe target depends on it), so
    # the app's defaults are not available: the keys used here must be in the file itself.
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) { Say "config file for the build not found: $configPath (CMake cache variable TAIDAFLOW_NGINX_CONFIG)"; exit 2 }
    try {
        $text = [System.IO.File]::ReadAllText($configPath, (New-Object System.Text.UTF8Encoding($false))).TrimStart([char]0xFEFF)
        $rootObj = $text | ConvertFrom-Json -ErrorAction Stop
    } catch { Say "config file for the build is not valid JSON: $configPath ($(($_.Exception.Message -split "`r?`n")[0]))"; exit 2 }
    $cfg = [pscustomobject]@{ Path = $configPath; Exists = $true; Created = $false; Error = ''; Values = @{}; Sources = @{}
                              Notes = (New-Object System.Collections.Generic.List[string]); DataDir = ''; BaseDir = (Split-Path -Parent $configPath) }
    foreach ($k in 'dataDir', 'nginx.enabled', 'nginx.port', 'rest.port', 'mirror.internalPort', 'mirror.publicPort', 'log.dir') {
        $o = $rootObj
        foreach ($seg in $k.Split('.')) { $prop = if ($o) { $o.PSObject.Properties[$seg] } else { $null }; $o = if ($prop) { $prop.Value } else { $null } }
        $t = Test-TaidaFlowValue $k $o
        if (-not $t.ok) { Say "config file for the build: `"$k`" missing or invalid in $configPath ($($t.reason)) - the build needs it in the file"; exit 2 }
        $cfg.Values[$k] = $t.value; $cfg.Sources[$k] = 'file'
    }
    Update-TaidaFlowDataDir $cfg
    if ($cfg.Error) { Say $cfg.Error; exit 2 }
} else {
    $cfg = Get-TaidaFlowConfig -Path $configPath -Exe $appExe -Create
}
Say "config.json: $configPath$(if ($cfg.Created) { ' (CREATED now with the default values)' })"
foreach ($n in $cfg.Notes) { Say "  config: $n" }
if ($cfg.Error) { Say "config.json unusable: $($cfg.Error)"; exit 2 }
$v = $cfg.Values
if ($NginxDir -ne "") {
    $nginxDir = Full $NginxDir
    $nginxExe = Join-Path $nginxDir 'nginx.exe'
} else {
    $nginxExe = if ($Nginx -ne "") { $x = Full $Nginx; if (Test-Path $x -PathType Container) { Join-Path $x 'nginx.exe' } else { $x } } else { Resolve-TaidaFlowNginxExe $cfg }
    if (-not (Test-Path $nginxExe -PathType Leaf)) {
        Say "nginx.exe not found: $nginxExe (config.json nginx.exe = $($v['nginx.exe']), $($cfg.Sources['nginx.exe'])) - the package bundles it in <installation folder>\nginx"
        exit 2
    }
    $nginxDir = Split-Path -Parent $nginxExe
}
$haveExe = Test-Path $nginxExe -PathType Leaf
$confDir = Join-Path $nginxDir 'conf'
$confFile = Join-Path $confDir 'nginx.conf'
$web = Join-Path $InstallDir 'web'
$exports = Join-Path $cfg.DataDir 'exports'
$logDir = Resolve-TaidaFlowLogDir $cfg
Say "nginx      : $nginxDir$(if (-not $haveExe) { ' (no nginx.exe there - nginx -t skipped)' })"
Say "web root   : $web"
Say "exports    : $exports (config.json dataDir $($cfg.DataDir))"
Say "log folder : $logDir (config.json log.dir = $($v['log.dir']), $($cfg.Sources['log.dir']); nginx-error.log, nginx-access-YYYY-MM-DD.log)"
Say ("ports      : listen {0} (nginx.port), /api/ -> 127.0.0.1:{1} (rest.port), /mirror -> 127.0.0.1:{2} (mirror.internalPort)" -f $v['nginx.port'], $v['rest.port'], $v['mirror.internalPort'])
if (-not $v['nginx.enabled']) { Say "NOTE: config.json nginx.enabled is false - the file is written, but start-taidaflow.ps1 will not start nginx and the app points pages at its own ports" }
if (-not (Test-Path (Join-Path $web 'TaidaFlowApp.html') -PathType Leaf)) {
    if ($Build) { Say "note: $web\TaidaFlowApp.html not deployed yet (scripts\deploy-web.ps1)" }
    else { Say "web page missing: $web\TaidaFlowApp.html"; exit 2 }
}

# Symbolic links / junctions (links are not followed).
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
if (-not $Build -and -not (Test-Path $exports -PathType Container)) { New-Item -ItemType Directory -Force $exports | Out-Null; Say "export folder created: $exports (the app creates the same folder at start-up)" }
if ($logDir -match '[\$"''{};#]' -or $logDir -match '[\x00-\x1f]') { Say "log folder path contains a character that cannot be used in the nginx configuration: $logDir"; exit 2 }
# nginx does not create folders: without it nginx -t / start fail on nginx-error.log (w2-065).
if (-not (Test-Path -LiteralPath $logDir -PathType Container)) {
    try { New-Item -ItemType Directory -Force $logDir | Out-Null; Say "log folder created: $logDir (the app writes its own log files there too)" }
    catch { Say "log folder cannot be created: $logDir - $($_.Exception.Message)"; exit 2 }
}
foreach ($pair in @(@('web root', $web), @('export folder', $exports))) {
    if ($pair[1] -match '[\$"''{};#]' -or $pair[1] -match '[\x00-\x1f]') { Say "$($pair[0]) path contains a character that cannot be used in the nginx configuration: $($pair[1])"; exit 2 }
    if (-not (Test-Path $pair[1] -PathType Container)) { continue }
    $links = Find-ReparsePoints $pair[1]
    if ($links.Count -gt 0) {
        Say "REFUSED: the $($pair[0]) $($pair[1]) is or holds $($links.Count) symbolic link(s) / junction(s) - nginx for Windows would serve files outside it:"
        $links | ForEach-Object { Say "    $_" }
        Say "  remove them (rmdir <junction> removes only the link) - nothing written"
        exit 5
    }
}

# --- write conf\nginx.conf -------------------------------------------------------------------------
# Header (Mango A9): generator version, the config.json used (path, last write time, SHA-256) and the
# installation folder. Deterministic - no time stamp of the run - so "-IfChanged" can compare.
$generatorVersion = 'install-nginx-config.ps1 v2 (w2-065: logs in log.dir)'
$marker = '# TAIDAFLOW NGINX CONFIGURATION'
try {
    $body = Get-TaidaFlowNginxConf $template @{ WebRoot = $web; ExportDir = $exports; LogDir = $logDir; Port = [int]$v['nginx.port']; RestPort = [int]$v['rest.port']
                                                MirrorPort = [int]$v['mirror.internalPort']; Prefix = $nginxDir }
} catch { Say $_.Exception.Message; exit 2 }
$cfgItem = Get-Item -LiteralPath $configPath
$cfgHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash
$by = if ($Build) { 'the desktop build (CMake target taidaflow_nginx_conf)' } else { 'install-nginx-config.ps1' }
$header = "$marker - GENERATED by $by from deploy\nginx\taidaflow.conf - run the generator again instead of editing this file.`n" +
          "# generator      : $generatorVersion`n" +
          "# config.json    : $configPath`n" +
          "# config written : $($cfgItem.LastWriteTimeUtc.ToString('yyyy-MM-ddTHH:mm:ssZ'))   SHA-256 $cfgHash`n" +
          "# install folder : $InstallDir`n" +
          "# nginx folder   : $nginxDir (prefix: start with  cd <nginx folder>  +  start nginx ; nginx -s reload ; nginx -s quit)`n" +
          "# web root       : $web (written relative to the nginx folder)`n" +
          "# export folder  : $exports`n" +
          "# log folder     : $logDir (config.json log.dir: nginx-error.log, nginx-access-YYYY-MM-DD.log)`n" +
          "# ports          : listen $($v['nginx.port'])   /api/ -> 127.0.0.1:$($v['rest.port'])   /mirror -> 127.0.0.1:$($v['mirror.internalPort'])`n"
$text = $header + $body
New-Item -ItemType Directory -Force $confDir, (Join-Path $nginxDir 'logs'), (Join-Path $nginxDir 'temp') | Out-Null
$changed = $true
if (Test-Path $confFile -PathType Leaf) {
    $old = [System.IO.File]::ReadAllText($confFile)
    if ($old -ceq $text) {
        $changed = $false
        Say "nginx.conf is up to date: $confFile"
    } elseif ($Build -and $old.StartsWith($marker)) {
        # -Build: an older generated file of the build folder is a build output - replaced without a backup
        Say "older generated nginx.conf replaced (build output, no backup): $confFile"
    } else {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $backup = if ($old.StartsWith($marker)) { "$confFile.prev-$stamp" } else { "$confFile.orig-$stamp" }
        Copy-Item -LiteralPath $confFile -Destination $backup
        Say "existing nginx.conf ($(if ($old.StartsWith($marker)) { 'an older TaidaFlow one' } else { 'not TaidaFlow' })) kept as $backup"
    }
}
if ($changed) {
    [System.IO.File]::WriteAllText($confFile, $text, (New-Object System.Text.UTF8Encoding($false)))
    Say "written: $confFile"
}
Say "NGINX_CONF=$(if ($changed) { 'written' } else { 'unchanged' })"
if (-not $Build) { Say ("  " + (Write-TaidaFlowRuntimeJson $web (Get-TaidaFlowPagePort $cfg))) }
if (-not $changed -and $IfChanged) { exit 0 }


# --- nginx -t with the default prefix (= the nginx folder, like "start nginx") ------------------------
if ($haveExe) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $nginxExe
    $psi.Arguments = '-t'
    $psi.WorkingDirectory = $nginxDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $testOut = $out + $errTask.Result
    foreach ($line in ($testOut -split "`r?`n")) { if ($line.Trim() -ne '') { Say "  nginx -t | $line" } }
    if ($p.ExitCode -ne 0) { Say "nginx -t FAILED (exit $($p.ExitCode)) - fix and run this script again"; exit 3 }
} else { Say "nginx -t skipped: no nginx.exe in $nginxDir" }
if ($Build) { exit 0 }

# --- port / running nginx (information) ---------------------------------------------------------------
$port = [int]$v['nginx.port']
$running = @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { try { [string]::Equals($_.Path, $nginxExe, [System.StringComparison]::OrdinalIgnoreCase) } catch { $false } })
$rc = 0
foreach ($l in @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)) {
    $owner = try { $pp = Get-Process -Id $l.OwningProcess -ErrorAction Stop; "$($pp.ProcessName) $($pp.Path)".Trim() } catch { '?' }
    if ($l.OwningProcess -eq 4) { $owner = 'System = Windows HTTP.sys (IIS, WinRM, WebDAV, a URL reservation ...; netsh http show servicestate)' }
    $ours = @($running | Where-Object { $_.Id -eq $l.OwningProcess }).Count -gt 0
    Say ("port {0}: listener {1}:{0} pid {2} ({3}){4}" -f $port, $l.LocalAddress, $l.OwningProcess, $owner, $(if ($ours) { ' = this nginx' } else { ' - ANOTHER program: nginx cannot listen on this port (not stopped by this script)' }))
    if (-not $ours) { $rc = 4 }
}
if ($running.Count) { Say "this nginx is running (pid $(($running | ForEach-Object { $_.Id }) -join ', ')): apply the new file with   cd `"$nginxDir`"   nginx -s reload" }
else { Say "nginx is not running: start-taidaflow.ps1 starts it, or by hand:   cd `"$nginxDir`"   start nginx" }
exit $rc

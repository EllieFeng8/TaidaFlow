# =====================================================================================
#  TaidaFlow - FIELD (production machine) start script.            (w2-057)
#
#  !!! THIS SCRIPT IS FOR THE PLANT. IT CONNECTS TO REAL EQUIPMENT. !!!
#  TaidaFlowApp.exe connects to the five ADAM modules at 192.168.1.201..205:502 and WRITES
#  DO/AO (pumps, valves, VFD, emergency-stop circuit), opens COM2 (MS300 inverter, Modbus RTU)
#  and runs a Modbus TCP server on 0.0.0.0:502 for the external HMI.
#  Its REST API (w2-060, 127.0.0.1:<RestPort>, on the LAN http://<IP>/api/... through nginx) can
#  change settings (PUT) - no access control (intranet).
#  Unlike the development script scripts\run-desktop.ps1 it does NOT run the development
#  safety probe (scripts\safety_probe.ps1 refuses exactly the plant situation on purpose).
#  Never run this on a development PC that can reach 192.168.1.201..205; there use
#  scripts\run-desktop.ps1 from the repository instead. The two sets of scripts must not be mixed.
# =====================================================================================
#
# Usage (from the installation folder, e.g. C:\TaidaFlow):
#   powershell -NoProfile -ExecutionPolicy Bypass -File start-taidaflow.ps1
#       [-DataDir <folder>] [-LogDir <folder>] [-UseNginx[:$false]] [-Port 80] [-Nginx <nginx.exe or folder>]
#       [-RestPort 18080] [-AppLog quiet|full] [-KeepLogDays 30] [-StartTimeoutSec 60]
#
# Site configuration (w2-060): config.json next to TaidaFlowApp.exe - the ONLY site configuration
# file (see DEPLOY.md section 1.2 and scripts\taidaflow-config.ps1):
#   {"dataDir": "C:\\TaidaFlowData", "useNginx": true, "nginxPort": 80, "restPort": 18080}
# Every value comes from there; a command-line parameter overrides it for this one run.
# Without config.json (or without a key) the built-in default below is used. An unreadable
# config.json or a wrong value -> exit 2, nothing started.
#
#   -DataDir     : working directory of the app. config.json "dataDir"; default (no config.json /
#                  no key) <installation folder>\runtime. The app writes TaidaFlowSettings.ini,
#                  settings.sqlite, device_info.ini, data\sensor_YYYYMM.sqlite and exports\ there.
#                  Created at the first start when missing; must be writable. The app also gets it
#                  as TAIDAFLOW_DATA_DIR (which the app prefers to config.json), so a -DataDir of
#                  this run is the folder the app really uses.
#   -LogDir      : log folder (default <DataDir>\logs): launcher.log (this script and
#                  stop-taidaflow.ps1) and one taidaflow-<yyyyMMdd-HHmmss>.log per app start.
#   -UseNginx    : also start nginx on 0.0.0.0:<Port> (web page + CSV downloads with resume + REST
#                  /api/) and start the app with TAIDAFLOW_DOWNLOAD_PORT=<Port>. Without it the
#                  download links use the app's own port 8124. config.json "useNginx"; default off.
#                  -UseNginx:$false switches it off for one run.
#   -Port        : nginx port. config.json "nginxPort"; default 80 (http://<IP>/).
#                  Only used with -UseNginx. nginx: <installation folder>\nginx\nginx.exe when
#                  the package contains it, otherwise -Nginx / TAIDAFLOW_NGINX / C:\tools\nginx.
#                  nginx runtime folder (config, logs, pid): <DataDir>\nginx.
#   -RestPort    : internal port of the app's REST API (RESTManager, 127.0.0.1 only). config.json
#                  "restPort"; default 18080. The app gets it as TAIDAFLOW_REST_PORT, nginx proxies
#                  http://<IP>/api/ to it.
#   -AppLog      : quiet (default) = only warnings and errors of the app go to the log (the app
#                  logs every Modbus read at info level: measured about 15 MB per hour);
#                  full = everything (for troubleshooting, watch the disk).
#   -KeepLogDays : app logs (taidaflow-*.log) older than this are deleted at start (0 = keep all).
#
# What it does:
#   1. Refuses to start (exit 4, nothing started, nothing stopped) when a TaidaFlowApp process is
#      already running or a port is already in use: 502 (Modbus server), 8124 (web + downloads),
#      8125 (web mirror relay), 18125 (internal mirror), the REST port (18080) and, with -UseNginx,
#      the nginx port (80).
#      For every busy port the owner is listed (pid 4 "System" = Windows HTTP.sys, e.g. IIS / WinRM /
#      a URL reservation; see DEPLOY.md). Programs of other people are never stopped.
#   2. Cleans the environment of the app: TAIDAFLOW_DEVICE_PROFILE (test mode) is ALWAYS removed,
#      TAIDAFLOW_DATA_DIR = the data folder, TAIDAFLOW_REST_PORT = the REST port, TAIDAFLOW_WEB_DIR
#      is removed (the page is always
#      <installation folder>\web), Qt variables (QT_PLUGIN_PATH, QML_IMPORT_PATH, ...) are removed
#      and PATH = installation folder + the system PATH without any folder holding Qt6Core.dll.
#      The Qt DLLs, plugins and QML modules come from the installation folder only.
#   3. (-UseNginx) starts nginx through scripts\nginx-web.ps1 (web root <installation folder>\web,
#      export folder <DataDir>\exports, /api/ -> 127.0.0.1:<RestPort>). If nginx cannot start, the app is still started, with
#      download links on 8124, and the script ends with exit 8.
#   4. Starts TaidaFlowApp.exe in <DataDir>, stderr -> <LogDir>\taidaflow-<time>.log, and waits
#      until the app listens on 8124 and 8125 (502, 18125 and the REST port are reported too).
#   5. Writes <DataDir>\taidaflow-app.json (pid, image, start time) for stop-taidaflow.ps1.
#
# Exit codes: 0 started; 2 package incomplete / folder not usable / config.json unusable; 4 refused (already running or
# port in use); 6 the app exited during start-up, or 8124/8125 not listening in time (the app is
# left running if it is alive - see the log); 8 app started but nginx did not (links use 8124).
param(
    [string]$DataDir = "",
    [string]$LogDir = "",
    [switch]$UseNginx,
    [ValidateRange(1, 65535)]
    [int]$Port = 80,
    [string]$Nginx = "",
    [ValidateRange(1, 65535)]
    [int]$RestPort = 18080,
    [ValidateSet('quiet', 'full')]
    [string]$AppLog = 'quiet',
    [int]$KeepLogDays = 30,
    [int]$StartTimeoutSec = 60
)
$ErrorActionPreference = 'Stop'
$install = [System.IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$exe = Join-Path $install 'TaidaFlowApp.exe'
$webRoot = Join-Path $install 'web'

function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $install $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
# --- site configuration: config.json (w2-060) -----------------------------------------------
$cfgReader = Join-Path $install 'scripts\taidaflow-config.ps1'
if (-not (Test-Path $cfgReader -PathType Leaf)) { [Console]::Out.WriteLine("ERROR: $cfgReader missing - incomplete package"); exit 2 }
. $cfgReader
$cfg = Read-TaidaFlowConfig $install
if ($cfg.Error) { [Console]::Out.WriteLine("ERROR: $($cfg.Error) - nothing started"); exit 2 }
$from = @{}
if ($PSBoundParameters.ContainsKey('DataDir') -and $DataDir -ne '') { $from.DataDir = '-DataDir' }
elseif ($cfg.DataDir) { $DataDir = $cfg.DataDir; $from.DataDir = 'config.json' }
else { $DataDir = Join-Path $install 'runtime'; $from.DataDir = 'default (no dataDir in config.json)' }
if ($PSBoundParameters.ContainsKey('UseNginx')) { $from.UseNginx = '-UseNginx' }
elseif ($null -ne $cfg.UseNginx) { $UseNginx = [switch]$cfg.UseNginx; $from.UseNginx = 'config.json' }
else { $from.UseNginx = 'default' }
if ($PSBoundParameters.ContainsKey('Port')) { $from.Port = '-Port' }
elseif ($cfg.NginxPort) { $Port = $cfg.NginxPort; $from.Port = 'config.json' }
else { $from.Port = 'default' }
if ($PSBoundParameters.ContainsKey('RestPort')) { $from.RestPort = '-RestPort' }
elseif ($cfg.RestPort) { $RestPort = $cfg.RestPort; $from.RestPort = 'config.json' }
else { $from.RestPort = 'default' }
$DataDir = Full $DataDir
if ($LogDir -eq "") { $LogDir = Join-Path $DataDir 'logs' }
$LogDir = Full $LogDir
$nginxRuntime = Join-Path $DataDir 'nginx'
$stateFile = Join-Path $DataDir 'taidaflow-app.json'

$script:launcherLog = $null
function Log([string]$m) {
    $line = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + ' [start] ' + $m
    [Console]::Out.WriteLine($line)
    if ($script:launcherLog) {
        try { [System.IO.File]::AppendAllText($script:launcherLog, $line + "`r`n", (New-Object System.Text.UTF8Encoding($false))) } catch { }
    }
}

# --- folders ------------------------------------------------------------------------
foreach ($d in $DataDir, $LogDir) {
    try {
        New-Item -ItemType Directory -Force $d | Out-Null
        $probe = Join-Path $d ('.write-test-' + [guid]::NewGuid().ToString('N'))
        [System.IO.File]::WriteAllText($probe, 'x')
        Remove-Item -LiteralPath $probe -Force
    } catch {
        [Console]::Out.WriteLine("folder not usable (create/write failed): $d - $($_.Exception.Message)")
        exit 2
    }
}
$script:launcherLog = Join-Path $LogDir 'launcher.log'
Log "=== TaidaFlow FIELD start (connects to the real equipment; no development safety probe) ==="
Log "installation: $install"
Log "data folder : $DataDir"
Log "log folder  : $LogDir"
Log ("config.json : {0}" -f $(if ($cfg.Found) { $cfg.Path } else { "$($cfg.Path) not found - built-in defaults" }))
foreach ($n in $cfg.Notes) { Log "config.json : $n" }
Log ("settings    : dataDir={0} ({1}); useNginx={2} ({3}); nginxPort={4} ({5}); restPort={6} ({7})" -f $DataDir, $from.DataDir,
     [bool]$UseNginx, $from.UseNginx, $Port, $from.Port, $RestPort, $from.RestPort)

if (-not (Test-Path $exe -PathType Leaf)) { Log "ERROR: $exe not found - incomplete package"; exit 2 }
foreach ($required in 'Qt6Core.dll', 'Qt6Quick.dll', 'platforms\qwindows.dll', 'qml\QtQuick\qmldir') {
    if (-not (Test-Path (Join-Path $install $required))) { Log "ERROR: $required missing in $install - incomplete package"; exit 2 }
}
if (-not (Test-Path (Join-Path $webRoot 'TaidaFlowApp.html') -PathType Leaf)) {
    Log "WARNING: $webRoot\TaidaFlowApp.html missing - the web page will not be served (the desktop HMI is not affected)"
}

# --- single instance / ports ----------------------------------------------------------
$refuse = $false
$running = @(Get-Process -Name TaidaFlowApp -ErrorAction SilentlyContinue)
foreach ($p in $running) {
    $path = try { $p.Path } catch { '?' }
    Log "REFUSED: TaidaFlowApp is already running (pid $($p.Id), $path) - close it first (stop-taidaflow.ps1); it is not stopped by this script"
    $refuse = $true
}
if (@(502, 8124, 8125, 18125) -contains $RestPort) { Log "ERROR: -RestPort $RestPort is another port of the app - choose another REST port"; exit 2 }
$ports = @(502, 8124, 8125, 18125, $RestPort)
if ($UseNginx) {
    if (@(502, 8124, 8125, 18125, $RestPort) -contains $Port) { Log "ERROR: -Port $Port is used by the app itself - choose another nginx port"; exit 2 }
    $ports += $Port
}
$listeners = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
foreach ($l in $listeners) {
    $owner = try { $pp = Get-Process -Id $l.OwningProcess -ErrorAction Stop; "$($pp.ProcessName) $($pp.Path)".Trim() } catch { '?' }
    if ($l.OwningProcess -eq 4) { $owner = 'System = Windows HTTP.sys (IIS, WinRM, WebDAV, a URL reservation ...; check: netsh http show servicestate)' }
    Log ("REFUSED: port {0} already in use ({1}:{0}, pid {2}, {3}) - the owner is not stopped" -f $l.LocalPort, $l.LocalAddress, $l.OwningProcess, $owner)
    $refuse = $true
}
if ($refuse) { Log "nothing started (exit 4)"; exit 4 }
Log ("ports free: " + ($ports -join ', '))

# --- log retention ----------------------------------------------------------------------
if ($KeepLogDays -gt 0) {
    $limit = (Get-Date).AddDays(-$KeepLogDays)
    $old = @(Get-ChildItem -LiteralPath $LogDir -File -Filter 'taidaflow-*.log*' -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match '^taidaflow-\d{8}-\d{6}\.log(\.stdout)?$' -and $_.LastWriteTime -lt $limit })
    foreach ($f in $old) { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue }
    if ($old.Count) { Log "deleted $($old.Count) app log file(s) older than $KeepLogDays days" }
}

# --- environment of the app -------------------------------------------------------------
foreach ($v in 'TAIDAFLOW_DEVICE_PROFILE', 'TAIDAFLOW_WEB_DIR', 'TAIDAFLOW_DOWNLOAD_PORT', 'TAIDAFLOW_REST_PORT', 'TAIDAFLOW_DATA_DIR',
               'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH',
               'QT_DEBUG_PLUGINS', 'QT_LOGGING_CONF', 'QT_QPA_PLATFORM') {
    if (Test-Path "Env:\$v") {
        Log "environment: removed $v (was '$((Get-Item "Env:\$v").Value)')"
        Remove-Item "Env:\$v"
    }
}
$kept = New-Object System.Collections.Generic.List[string]
foreach ($entry in ($env:PATH -split ';')) {
    $e = $entry.Trim()
    if ($e -eq '') { continue }
    $hasQt = $false
    try { $hasQt = Test-Path -LiteralPath (Join-Path $e 'Qt6Core.dll') } catch { $hasQt = $false }
    if ($hasQt) { Log "environment: PATH entry removed (holds Qt6Core.dll): $e"; continue }
    $kept.Add($e)
}
$env:PATH = $install + ';' + ($kept -join ';')
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:TAIDAFLOW_REST_PORT = "$RestPort"
Log "TAIDAFLOW_REST_PORT=$RestPort (REST API on 127.0.0.1:$RestPort)"
# The app prefers TAIDAFLOW_DATA_DIR to its own reading of config.json: the folder of this run
# (also a -DataDir override) is the one the app uses.
$env:TAIDAFLOW_DATA_DIR = $DataDir
Log "TAIDAFLOW_DATA_DIR=$DataDir"
if ($AppLog -eq 'quiet') {
    $rules = Join-Path $install 'logging\quiet.ini'
    if (Test-Path $rules -PathType Leaf) { $env:QT_LOGGING_CONF = $rules }
    else { Log "WARNING: $rules missing - app log stays full" }
}
Log "app log mode: $AppLog$(if ($env:QT_LOGGING_CONF) { " (QT_LOGGING_CONF=$env:QT_LOGGING_CONF)" })"

# --- nginx (optional) -------------------------------------------------------------------
$nginxOk = $false
$downloadPort = 8124
function Invoke-NginxWeb([string[]]$more) {
    $script = Join-Path $install 'scripts\nginx-web.ps1'
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script) + $more
    $out = & powershell.exe @a
    $rc = $LASTEXITCODE
    foreach ($line in @($out)) { if ("$line".Trim() -ne '') { Log "  nginx | $line" } }
    return $rc
}
if ($UseNginx) {
    $nginxArgs = @('-Action', 'start', '-Port', "$Port", '-WebRoot', $webRoot, '-ExportDir', (Join-Path $DataDir 'exports'),
                   '-RuntimeDir', $nginxRuntime, '-RestPort', "$RestPort")
    $nginxExe = ''
    if ($Nginx -ne '') { $nginxExe = Full $Nginx }
    elseif (Test-Path (Join-Path $install 'nginx\nginx.exe') -PathType Leaf) { $nginxExe = Join-Path $install 'nginx\nginx.exe' }
    if ($nginxExe -ne '') { $nginxArgs += @('-Nginx', $nginxExe) }
    Log "starting nginx (scripts\nginx-web.ps1 -Action start, runtime $nginxRuntime)"
    $rc = Invoke-NginxWeb $nginxArgs
    if ($rc -eq 0) {
        $nginxOk = $true
        $downloadPort = $Port
        Log "nginx started (0.0.0.0:$Port)"
    } else {
        Log "WARNING: nginx NOT started (nginx-web.ps1 exit $($rc): 2 missing nginx/web page, 3 nginx -t failed, 4 port $Port busy, 5 link/junction in web or export folder, 6 not listening in time) - the app is started with download links on 8124"
    }
}
$env:TAIDAFLOW_DOWNLOAD_PORT = "$downloadPort"
Log "TAIDAFLOW_DOWNLOAD_PORT=$downloadPort"

# --- start the app ----------------------------------------------------------------------
$appLogFile = Join-Path $LogDir ('taidaflow-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
try {
    $p = Start-Process -FilePath $exe -WorkingDirectory $DataDir -PassThru `
            -RedirectStandardError $appLogFile -RedirectStandardOutput "$appLogFile.stdout"
    $null = $p.Handle
} catch {
    Log "ERROR: TaidaFlowApp could not be started: $($_.Exception.Message)"
    if ($nginxOk) {
        Log "stopping the nginx started above"
        $null = Invoke-NginxWeb @('-Action', 'stop', '-RuntimeDir', $nginxRuntime)
    }
    exit 6
}
$state = [ordered]@{
    pid = $p.Id; exe = $exe; startTicksUtc = $p.StartTime.ToUniversalTime().Ticks; startTime = $p.StartTime.ToString('o')
    dataDir = $DataDir; logDir = $LogDir; appLog = $appLogFile; useNginx = [bool]$UseNginx; nginxStarted = $nginxOk
    nginxRuntime = $nginxRuntime; nginxPort = $(if ($UseNginx) { $Port } else { 0 }); downloadPort = $downloadPort
    restPort = $RestPort
}
[System.IO.File]::WriteAllText($stateFile, ($state | ConvertTo-Json), (New-Object System.Text.UTF8Encoding($false)))
Log "TaidaFlowApp started: pid $($p.Id), log $appLogFile"

$t0 = Get-Date
$need = @(8124, 8125)
$have = @()
do {
    Start-Sleep -Milliseconds 500
    $have = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
              Where-Object { $_.OwningProcess -eq $p.Id -and (@(502, 8124, 8125, 18125, $RestPort) -contains $_.LocalPort) } |
              ForEach-Object { [int]$_.LocalPort } | Sort-Object -Unique)
    $missing = @($need | Where-Object { $have -notcontains $_ })
} while ($missing.Count -gt 0 -and -not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $StartTimeoutSec)

if ($p.HasExited) {
    Log "ERROR: TaidaFlowApp exited during start-up (exit code $($p.ExitCode)); last log lines:"
    if (Test-Path $appLogFile) { Get-Content -LiteralPath $appLogFile -Tail 15 | ForEach-Object { Log "  app | $_" } }
    Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
    if ($nginxOk) {
        Log "stopping the nginx started above"
        $null = Invoke-NginxWeb @('-Action', 'stop', '-RuntimeDir', $nginxRuntime)
    }
    exit 6
}
foreach ($pp in 502, 8124, 8125, 18125, $RestPort) {
    $what = @{ 502 = 'Modbus server (external HMI)'; 8124 = 'web page + CSV downloads'; 8125 = 'web mirror relay (LAN)'; 18125 = 'internal mirror (127.0.0.1)' }[$pp]
    if ($pp -eq $RestPort) { $what = 'REST API (127.0.0.1; LAN via nginx /api/)' }
    Log ("  port {0,-5} {1,-30} {2}" -f $pp, $what, $(if ($have -contains $pp) { 'listening' } else { 'NOT listening' }))
}
if ($missing.Count -gt 0) {
    Log "ERROR: after $StartTimeoutSec s the app does not listen on $($missing -join ', ') - the app is left running (desktop HMI); see $appLogFile"
    exit 6
}
if ($have -notcontains $RestPort) { Log "WARNING: the REST API does not listen on 127.0.0.1:$RestPort (see [REST] in $appLogFile); the app runs without it" }
$ips = @(try { Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop | Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' } | ForEach-Object { $_.IPAddress } } catch { })
foreach ($ip in @($ips) + @('127.0.0.1')) {
    if ($nginxOk) {
        $base = if ($Port -eq 80) { "http://${ip}" } else { "http://${ip}:$Port" }
        Log "web page: $base/  (nginx -> /TaidaFlowApp.html; also http://${ip}:8124/TaidaFlowApp.html)  REST API: $base/api/"
    }
    else { Log "web page: http://${ip}:8124/TaidaFlowApp.html" }
}
if ($UseNginx -and -not $nginxOk) { Log "started WITHOUT nginx (exit 8)"; exit 8 }
Log "started (exit 0)"
exit 0

# =====================================================================================
#  TaidaFlow - FIELD (production machine) start script.            (w2-057, w2-062, w2-065)
#
#  !!! THIS SCRIPT IS FOR THE PLANT. IT CONNECTS TO REAL EQUIPMENT. !!!
#  TaidaFlowApp.exe connects to the five ADAM modules of config.json (devices.adam*, plant default
#  192.168.1.201..205:502) and WRITES DO/AO (pumps, valves, VFD, emergency-stop circuit), opens the
#  MS300 serial port (devices.ms300, COM2) and runs a Modbus TCP server (modbusServer, 0.0.0.0:502)
#  for the external HMI. Its REST API (rest, 127.0.0.1:18080; on the LAN http://<IP>/api/... through
#  nginx) can change settings (PUT) - no access control (intranet).
#  Unlike the development script scripts\run-desktop.ps1 it does NOT run the development safety
#  probe (scripts\safety_probe.ps1 refuses exactly the plant situation on purpose). Never run this on
#  a development PC that can reach the plant devices; there use scripts\run-desktop.ps1 from the
#  repository. The two sets of scripts must not be mixed.
# =====================================================================================
#
# w2-062: ALL settings come from config.json (docs/taidaflow_config_spec.md), by default the file next
# to TaidaFlowApp.exe in this folder. The same file is used by the app (TAIDAFLOW_CONFIG is set to it),
# by the nginx configuration (scripts\install-nginx-config.ps1) and by stop-taidaflow.ps1. A missing config.json is created with
# the defaults by  TaidaFlowApp.exe --write-default-config <path>  (never overwritten; a package never
# contains one, so an update keeps the plant's file). A config.json that is not valid JSON: nothing is
# started (exit 2), the file is not changed - fix it or delete it to get the defaults again.
#
# Logs (w2-065, spec section 2 "log"): everything is in ONE folder, config.json log.dir resolved against
# dataDir (default C:\TaidaFlowData\logs):
#   taidaflow-YYYY-MM-DD.log       written by the app itself: warnings and errors (kept log.quiet.keepDays, 60)
#   taidaflow-YYYY-MM-DD-full.log  written by the app itself: every message (kept log.full.keepDays, 7)
#                                  (the app deletes its own expired files at start and at midnight)
#   launcher-YYYY-MM-DD.log        this script and stop-taidaflow.ps1 (one line per step)
#   nginx-access-YYYY-MM-DD.log    nginx, one line per request; nginx-error.log  nginx warnings / errors
# This script deletes launcher-YYYY-MM-DD.log and nginx-access-YYYY-MM-DD.log files older than
# log.quiet.keepDays days (today included) at every start - only names that match exactly. It no longer
# redirects the app's output into a log file and does not filter the app's messages (no
# QT_LOGGING_CONF / QT_LOGGING_RULES: they would remove the info lines from the full file). Files of
# older versions (taidaflow-yyyyMMdd-HHmmss.log(.stdout), launcher.log) are never deleted by anybody.
#
# Usage (from the installation folder, e.g. C:\TaidaFlow\TaidaFlow-<date>-<hash>):
#   powershell -NoProfile -ExecutionPolicy Bypass -File start-taidaflow.ps1
#       [-Config <config.json>] [-StartTimeoutSec 60]
#       one-time overrides of config.json (this start only; the file is not changed):
#       [-DataDir <folder>] [-LogDir <folder>] [-UseNginx | -NoNginx] [-Port <nginx port>] [-RestPort <port>]
#       [-Nginx <nginx.exe or folder>]
#
#   -Config      : another config.json (default <installation folder>\config.json).
#   -DataDir     : data folder instead of config.json dataDir (default C:\TaidaFlowData). The app works
#                  there: TaidaFlowSettings.ini, settings.sqlite, data\sensor_YYYYMM.sqlite, exports\,
#                  and (log.dir "logs") logs\. Created when missing; must be writable.
#   -LogDir      : log folder instead of config.json log.dir (see "Logs" above). Like the other
#                  overrides it is given to the app AND to nginx, so all log files stay in one folder.
#                  stop-taidaflow.ps1 finds it in <data folder>\taidaflow-app.json.
#   -UseNginx / -NoNginx, -Port, -RestPort, -Nginx : override nginx.enabled, nginx.port, rest.port,
#                  nginx.exe. The app gets the overrides through a generated complete copy of the
#                  configuration, <data folder>\config.effective.json (TAIDAFLOW_CONFIG points to it;
#                  it is rewritten at every start that needs it and is never read by anyone else).
#
# What it does:
#   1. Reads config.json (creates it when missing). Refuses to start (exit 4, nothing started,
#      nothing stopped) when a TaidaFlowApp process is already running or a port of config.json is
#      already in use: modbusServer.port, http.port, mirror.publicPort, mirror.internalPort,
#      rest.port and, with nginx.enabled, nginx.port. For every busy port the owner is listed (pid 4
#      "System" = Windows HTTP.sys, e.g. IIS / WinRM / a URL reservation). Programs of other people are
#      never stopped.
#   2. Cleans the environment of the app: TAIDAFLOW_DEVICE_PROFILE (test mode) is ALWAYS removed, as
#      are TAIDAFLOW_WEB_DIR (the page is always <installation folder>\web), TAIDAFLOW_DOWNLOAD_PORT,
#      TAIDAFLOW_REST_PORT (no longer used), Qt variables (QT_PLUGIN_PATH, QML_IMPORT_PATH,
#      QT_LOGGING_CONF, QT_LOGGING_RULES, QT_FORCE_STDERR_LOGGING, ...);
#      PATH = installation folder + the system PATH without any folder holding Qt6Core.dll.
#   3. Deletes expired launcher-YYYY-MM-DD.log / nginx-access-YYYY-MM-DD.log (see "Logs").
#   4. (nginx.enabled; Mango A6/A7/A9) nginx = config.json nginx.exe (default nginx\nginx.exe, the one
#      bundled in this folder). Its conf\nginx.conf is checked with scripts\install-nginx-config.ps1
#      -IfChanged: missing, written for another installation folder / config.json, or changed -> it is
#      regenerated (old file kept as nginx.conf.prev-<time> / .orig-<time>) and a running nginx gets
#      "nginx -s reload". A running nginx is left as it is; otherwise it is started exactly like
#      "cd <nginx folder>" + "start nginx" (no arguments). stop-taidaflow.ps1 does NOT stop nginx
#      ("nginx -s quit" in its folder). If nginx is not available, the app is still started with
#      nginx.enabled=false (download links and runtime.json then use http.port / mirror.publicPort of
#      the app itself) and the script ends with exit 8.
#   5. Starts TaidaFlowApp.exe in the data folder without a console window (the app writes its own log
#      files; w2-065) and waits until the app listens on http.port and mirror.publicPort (the others are
#      reported too). What the app writes straight to stderr/stdout during these seconds (normally
#      nothing: only a problem of its own log files would be written there) is copied into the
#      launcher log; after the start nothing of the app goes through this script any more.
#   6. Writes <data folder>\taidaflow-app.json (pid, image, start time, log folder) for stop-taidaflow.ps1.
#
# Exit codes: 0 started; 2 package incomplete / folder not usable / config.json unusable; 4 refused
# (already running or port in use); 6 the app exited during start-up, or its ports were not listening
# in time (the app is left running if it is alive - see the log); 8 app started but nginx did not.
param(
    [string]$Config = "",
    [string]$DataDir = "",
    [string]$LogDir = "",
    [switch]$UseNginx,
    [switch]$NoNginx,
    [ValidateRange(1, 65535)]
    [int]$Port = 80,
    [ValidateRange(1, 65535)]
    [int]$RestPort = 18080,
    [string]$Nginx = "",
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
# launcher-YYYY-MM-DD.log in the log folder (the date of each line); lines before the folder is known
# are kept in memory and written first.
$script:launcherDir = $null
$script:pending = New-Object System.Collections.Generic.List[string]
function Get-LauncherLog { if ($script:launcherDir) { return (Join-Path $script:launcherDir ('launcher-' + (Get-Date).ToString('yyyy-MM-dd') + '.log')) } return $null }
function Log([string]$m) {
    $line = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') + ' [start] ' + $m
    [Console]::Out.WriteLine($line)
    if ($script:launcherDir) {
        try { [System.IO.File]::AppendAllText((Get-LauncherLog), $line + "`r`n", (New-Object System.Text.UTF8Encoding($false))) } catch { }
    } else { $script:pending.Add($line) }
}

$helper = Join-Path $install 'scripts\taidaflow-config.ps1'
if (-not (Test-Path $exe -PathType Leaf) -or -not (Test-Path $helper -PathType Leaf)) {
    [Console]::Out.WriteLine("ERROR: $exe or $helper not found - incomplete package"); exit 2
}
foreach ($required in 'Qt6Core.dll', 'Qt6Quick.dll', 'platforms\qwindows.dll', 'qml\QtQuick\qmldir') {
    if (-not (Test-Path (Join-Path $install $required))) { [Console]::Out.WriteLine("ERROR: $required missing in $install - incomplete package"); exit 2 }
}
. $helper
if ($UseNginx -and $NoNginx) { [Console]::Out.WriteLine("ERROR: -UseNginx and -NoNginx together"); exit 2 }

# --- environment of the app (also for the --write-default-config call below) -------------------
# QT_LOGGING_CONF / QT_LOGGING_RULES would filter messages BEFORE the app's log writer sees them (the
# full file would lose its info lines, w2-064); QT_FORCE_STDERR_LOGGING / QT_ASSUME_STDERR_HAS_CONSOLE
# would copy every message to stderr as well. The app decides itself what goes into which file.
$envNotes = New-Object System.Collections.Generic.List[string]
foreach ($v in 'TAIDAFLOW_DEVICE_PROFILE', 'TAIDAFLOW_WEB_DIR', 'TAIDAFLOW_DOWNLOAD_PORT', 'TAIDAFLOW_REST_PORT', 'TAIDAFLOW_CONFIG',
               'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH',
               'QT_DEBUG_PLUGINS', 'QT_LOGGING_CONF', 'QT_LOGGING_RULES', 'QT_FORCE_STDERR_LOGGING', 'QT_ASSUME_STDERR_HAS_CONSOLE',
               'QT_LOGGING_TO_CONSOLE', 'QT_QPA_PLATFORM') {
    if (Test-Path "Env:\$v") {
        $envNotes.Add("environment: removed $v (was '$((Get-Item "Env:\$v").Value)')")
        Remove-Item "Env:\$v"
    }
}
$kept = New-Object System.Collections.Generic.List[string]
foreach ($entry in ($env:PATH -split ';')) {
    $e = $entry.Trim()
    if ($e -eq '') { continue }
    $hasQt = $false
    try { $hasQt = Test-Path -LiteralPath (Join-Path $e 'Qt6Core.dll') } catch { $hasQt = $false }
    if ($hasQt) { $envNotes.Add("environment: PATH entry removed (holds Qt6Core.dll): $e"); continue }
    $kept.Add($e)
}
$env:PATH = $install + ';' + ($kept -join ';')

# --- config.json ------------------------------------------------------------------------------
$configPath = if ($Config -ne "") { Full $Config } else { Join-Path $install 'config.json' }
$cfg = Get-TaidaFlowConfig -Path $configPath -Exe $exe -Create
if ($cfg.Error) { [Console]::Out.WriteLine("ERROR: $($cfg.Error) - nothing started (exit 2)"); exit 2 }
$overrides = New-Object System.Collections.Generic.List[string]
try {
    if ($DataDir -ne "") { Set-TaidaFlowConfigValue $cfg 'dataDir' (Full $DataDir); $overrides.Add("dataDir=$($cfg.DataDir) (-DataDir)") }
    if ($LogDir -ne "") { Set-TaidaFlowConfigValue $cfg 'log.dir' (Full $LogDir); $overrides.Add("log.dir=$(Full $LogDir) (-LogDir)") }
    if ($UseNginx) { Set-TaidaFlowConfigValue $cfg 'nginx.enabled' $true; $overrides.Add('nginx.enabled=true (-UseNginx)') }
    if ($NoNginx) { Set-TaidaFlowConfigValue $cfg 'nginx.enabled' $false; $overrides.Add('nginx.enabled=false (-NoNginx)') }
    if ($PSBoundParameters.ContainsKey('Port')) { Set-TaidaFlowConfigValue $cfg 'nginx.port' $Port; $overrides.Add("nginx.port=$Port (-Port)") }
    if ($PSBoundParameters.ContainsKey('RestPort')) { Set-TaidaFlowConfigValue $cfg 'rest.port' $RestPort; $overrides.Add("rest.port=$RestPort (-RestPort)") }
    if ($Nginx -ne "") { Set-TaidaFlowConfigValue $cfg 'nginx.exe' (Full $Nginx); $overrides.Add("nginx.exe=$(Full $Nginx) (-Nginx)") }
} catch { [Console]::Out.WriteLine("ERROR: $($_.Exception.Message)"); exit 2 }
$DataDir = $cfg.DataDir
# The app's log folder (AppConfig::resolvedLogDir): log.dir relative to dataDir. The launcher log and
# nginx's logs go to the same folder.
$LogDir = Resolve-TaidaFlowLogDir $cfg
$keepDays = [int]$cfg.Values['log.quiet.keepDays']
$stateFile = Join-Path $DataDir 'taidaflow-app.json'
$effectiveConfig = Join-Path $DataDir 'config.effective.json'

# --- folders ------------------------------------------------------------------------------------
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
$script:launcherDir = $LogDir
Log "=== TaidaFlow FIELD start (connects to the real equipment; no development safety probe) ==="
foreach ($l in $script:pending) { try { [System.IO.File]::AppendAllText((Get-LauncherLog), $l + "`r`n", (New-Object System.Text.UTF8Encoding($false))) } catch { } }
Log "installation: $install"
Log "config.json : $configPath$(if ($cfg.Created) { ' (CREATED now with the default values)' })"
foreach ($n in $cfg.Notes) { Log "  config: $n" }
foreach ($o in $overrides) { Log "  one-time override: $o" }
Log "data folder : $DataDir"
Log ("log folder  : {0} (config.json log.dir = {1}, {2}; relative = to the data folder)" -f $LogDir, $cfg.Values['log.dir'], $cfg.Sources['log.dir'])
foreach ($n in $envNotes) { Log $n }
if (-not (Test-Path (Join-Path $webRoot 'TaidaFlowApp.html') -PathType Leaf)) {
    Log "WARNING: $webRoot\TaidaFlowApp.html missing - the web page will not be served (the desktop HMI is not affected)"
}
$v = $cfg.Values
Log ("devices: " + ((@('adam6256', 'adam6217a', 'adam6217b', 'adam6224', 'adam6022') | ForEach-Object { "$_ $($v["devices.$_.host"]):$($v["devices.$_.port"])" }) -join ', ') +
     "; MS300 $($v['devices.ms300.serialPort']) $($v['devices.ms300.baudRate'])")

# --- ports / single instance ------------------------------------------------------------------------
$portNames = [ordered]@{}
$portNames["$($v['modbusServer.port'])"] = "Modbus server (modbusServer.port, $($v['modbusServer.bind']))"
$useNginxNow = [bool]$v['nginx.enabled']
$pairs = @(@([int]$v['http.port'], 'web page + CSV downloads (http.port)'), @([int]$v['mirror.publicPort'], 'web mirror relay (mirror.publicPort)'),
           @([int]$v['mirror.internalPort'], 'internal mirror, 127.0.0.1 (mirror.internalPort)'), @([int]$v['rest.port'], 'REST API, 127.0.0.1 (rest.port)'))
if ($useNginxNow) { $pairs += ,@([int]$v['nginx.port'], 'nginx (nginx.port)') }
foreach ($pp in $pairs) {
    if ($portNames.Contains("$($pp[0])")) { Log "ERROR: port $($pp[0]) is used twice in config.json ($($portNames["$($pp[0])"]) and $($pp[1])) - fix config.json"; exit 2 }
    $portNames["$($pp[0])"] = $pp[1]
}
$ports = @($portNames.Keys | ForEach-Object { [int]$_ })
$refuse = $false
$running = @(Get-Process -Name TaidaFlowApp -ErrorAction SilentlyContinue)
foreach ($p in $running) {
    $path = try { $p.Path } catch { '?' }
    Log "REFUSED: TaidaFlowApp is already running (pid $($p.Id), $path) - close it first (stop-taidaflow.ps1); it is not stopped by this script"
    $refuse = $true
}
$listeners = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $ports -contains $_.LocalPort })
$cfgNginxExe = Resolve-TaidaFlowNginxExe $cfg
foreach ($l in $listeners) {
    $ownerPath = try { (Get-Process -Id $l.OwningProcess -ErrorAction Stop).Path } catch { '' }
    if ($useNginxNow -and $l.LocalPort -eq [int]$v['nginx.port'] -and [string]::Equals($ownerPath, $cfgNginxExe, [System.StringComparison]::OrdinalIgnoreCase)) {
        Log "port $($l.LocalPort) = the configured nginx, already running (pid $($l.OwningProcess)) - fine"
        continue
    }
    $owner = try { $pp = Get-Process -Id $l.OwningProcess -ErrorAction Stop; "$($pp.ProcessName) $($pp.Path)".Trim() } catch { '?' }
    if ($l.OwningProcess -eq 4) { $owner = 'System = Windows HTTP.sys (IIS, WinRM, WebDAV, a URL reservation ...; check: netsh http show servicestate)' }
    Log ("REFUSED: port {0} ({4}) already in use ({1}:{0}, pid {2}, {3}) - the owner is not stopped" -f $l.LocalPort, $l.LocalAddress, $l.OwningProcess, $owner, $portNames["$($l.LocalPort)"])
    $refuse = $true
}
if ($refuse) { Log "nothing started (exit 4)"; exit 4 }
Log ("ports free: " + (($ports | ForEach-Object { "$_ $($portNames["$_"])" }) -join ', '))

# --- log retention (w2-065) ------------------------------------------------------------------------
# The app deletes its own taidaflow-YYYY-MM-DD(-full).log. This script deletes only its own
# launcher-YYYY-MM-DD.log and nginx's nginx-access-YYYY-MM-DD.log, older than log.quiet.keepDays days
# (today included, same rule as the app). Nothing else in the folder is touched: nginx-error.log, the
# files of older versions (taidaflow-yyyyMMdd-HHmmss.log, .stdout, launcher.log), files of the operator.
foreach ($kind in @(@('launcher-', 'launcher log'), @('nginx-access-', 'nginx access log'))) {
    $r = Remove-TaidaFlowExpiredDatedFiles $LogDir $kind[0] '.log' $keepDays
    Log ("clean-up {0}: {1}YYYY-MM-DD.log older than {2} day(s) (log.quiet.keepDays, {3}): {4} deleted, {5} kept{6}" -f $kind[1], $kind[0], $keepDays,
         $cfg.Sources['log.quiet.keepDays'], $r.Deleted.Count, $r.Kept, $(if ($r.Deleted.Count) { ' (' + ($r.Deleted -join ', ') + ')' } else { '' }))
    foreach ($x in $r.Failed) { Log "WARNING: could not delete $x - left, tried again at the next start" }
}
$legacy = @(Get-ChildItem -LiteralPath $LogDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^taidaflow-\d{8}-\d{6}\.log(\.stdout)?$' })
if ($legacy.Count) { Log "note: $($legacy.Count) log file(s) of the older start script (taidaflow-yyyyMMdd-HHmmss.log) are kept - never deleted automatically; delete them by hand when no longer needed" }
$fullOn = [bool]$v['log.full.enabled']; $quietOn = [bool]$v['log.quiet.enabled']
Log ("app log files (written by the app): {0}\taidaflow-YYYY-MM-DD.log (warnings/errors) {1}, keep {2} day(s); taidaflow-YYYY-MM-DD-full.log (every message) {3}, keep {4} day(s)" -f
     $LogDir, $(if ($quietOn) { 'on' } else { 'OFF' }), $v['log.quiet.keepDays'], $(if ($fullOn) { 'on' } else { 'OFF' }), $v['log.full.keepDays'])

# The configuration file the app and nginx read: config.json itself, or (one-time overrides) a
# complete copy with the overrides applied.
function Set-AppConfigFile {
    if ($overrides.Count -gt 0) {
        Write-TaidaFlowConfigFile $cfg $effectiveConfig
        $env:TAIDAFLOW_CONFIG = $effectiveConfig
        Log "TAIDAFLOW_CONFIG=$effectiveConfig (config.json + one-time overrides: $($overrides -join '; '))"
    } else {
        $env:TAIDAFLOW_CONFIG = $configPath
        Log "TAIDAFLOW_CONFIG=$configPath"
    }
}
Set-AppConfigFile

# --- nginx (config.json nginx.enabled; Mango A6/A7) -----------------------------------------------
# nginx is independent of the app: started the standard way ("cd <nginx folder>" + "start nginx", no
# arguments) with <nginx folder>\conf\nginx.conf written once by scripts\install-nginx-config.ps1.
# Already running -> left as it is. stop-taidaflow.ps1 does not stop it ("nginx -s quit").
$nginxOk = $false
$nginxStartedNow = $false
$nginxProblem = ''
if ($useNginxNow) {
    $nginxExe = Resolve-TaidaFlowNginxExe $cfg
    $nginxDir = Split-Path -Parent $nginxExe
    $nginxConf = Join-Path $nginxDir 'conf\nginx.conf'
    $nginxPort = [int]$v['nginx.port']
    Log "nginx: $nginxExe (config.json nginx.exe = $($v['nginx.exe']), $($cfg.Sources['nginx.exe']); relative = to the folder of config.json)"
    $mine = @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { try { [string]::Equals($_.Path, $nginxExe, [System.StringComparison]::OrdinalIgnoreCase) } catch { $false } })
    $confOk = $false
    if (-not (Test-Path $nginxExe -PathType Leaf)) {
        $nginxProblem = "nginx.exe not found: $nginxExe"
    } else {
        # Mango A9: nginx.conf must belong to THIS installation folder and THIS config.json (the header
        # records both). Missing / older / moved / changed -> regenerated with the same generator as
        # the one-time install step (the old file is kept as nginx.conf.prev-<time> / .orig-<time>).
        $gen = Join-Path $install 'scripts\install-nginx-config.ps1'
        $genOut = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $gen -Config $env:TAIDAFLOW_CONFIG -InstallDir $install -Nginx $nginxExe -IfChanged
        $genRc = $LASTEXITCODE
        foreach ($line in @($genOut)) { if ("$line".Trim() -ne '') { Log "  nginx.conf | $line" } }
        $written = @($genOut | Where-Object { "$_" -match '^NGINX_CONF=written' }).Count -gt 0
        if ($genRc -eq 0 -or $genRc -eq 4) {
            $confOk = $true
            if ($written) { Log "nginx.conf regenerated for this installation / config.json (see above)" }
            if ($written -and $mine.Count) {
                Log "nginx is running with the old file - applying the new one: nginx -s reload (from $nginxDir)"
                $rp = Start-Process -FilePath $nginxExe -ArgumentList @('-s', 'reload') -WorkingDirectory $nginxDir -WindowStyle Hidden -PassThru
                $null = $rp.Handle
                if (-not $rp.WaitForExit(20000) -or $rp.ExitCode -ne 0) { Log "WARNING: nginx -s reload did not succeed (exit $(try { $rp.ExitCode } catch { '?' }))" }
            }
        } else {
            $nginxProblem = "nginx.conf could not be generated / checked (install-nginx-config.ps1 exit $genRc)"
        }
    }
    if ($confOk) {
        if ($mine.Count) {
            Log "nginx is already running (pid $(($mine | ForEach-Object { $_.Id }) -join ', ')) - left as it is"
        } else {
            Log "starting nginx like 'cd $nginxDir' + 'start nginx' (no arguments)"
            $np = Start-Process -FilePath $nginxExe -WorkingDirectory $nginxDir -WindowStyle Hidden -PassThru
            $null = $np.Handle
            $nginxStartedNow = $true
        }
        $t0 = Get-Date
        do {
            Start-Sleep -Milliseconds 250
            $ids = @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object { try { [string]::Equals($_.Path, $nginxExe, [System.StringComparison]::OrdinalIgnoreCase) } catch { $false } } | ForEach-Object { $_.Id })
            $l = @(Get-NetTCPConnection -State Listen -LocalPort $nginxPort -ErrorAction SilentlyContinue | Where-Object { $ids -contains $_.OwningProcess })
        } while ($l.Count -eq 0 -and ((Get-Date) - $t0).TotalSeconds -lt 20)
        if ($l.Count -gt 0) { $nginxOk = $true; Log "nginx listening on $($l[0].LocalAddress):$nginxPort (pid $($l[0].OwningProcess)); logs: $LogDir\nginx-access-YYYY-MM-DD.log, nginx-error.log" }
        else {
            $nginxProblem = "nginx does not listen on port $nginxPort within 20 s (see $LogDir\nginx-error.log and $nginxDir\logs\error.log)"
            foreach ($err in (Join-Path $LogDir 'nginx-error.log'), (Join-Path $nginxDir 'logs\error.log')) {
                if (Test-Path $err) { Get-Content -LiteralPath $err -Tail 5 | ForEach-Object { Log "  $(Split-Path -Leaf $err) | $_" } }
            }
        }
    }
    if (-not $nginxOk) {
        Log "WARNING: nginx NOT available ($nginxProblem) - the app is started with nginx.enabled=false (web page, downloads and mirror on its own ports $($v['http.port']) / $($v['mirror.publicPort']))"
        Set-TaidaFlowConfigValue $cfg 'nginx.enabled' $false
        $overrides.Add('nginx.enabled=false (nginx not available)')
        Set-AppConfigFile
    }
}

# --- start the app ----------------------------------------------------------------------------------
# w2-065: no console window (CreateNoWindow): the app writes its own log files, Qt then prints its
# messages only to the debugger output, and no console window can be closed by accident (closing it
# would end the app). stdout / stderr are read only while this script waits for the start-up.
$today = (Get-Date).ToString('yyyy-MM-dd')
$fullLog = Join-Path $LogDir "taidaflow-$today-full.log"
$quietLog = Join-Path $LogDir "taidaflow-$today.log"
# Collects what arrives on a redirected stream in the background (a .NET thread, no PowerShell
# runspace needed), so the text so far can be read while the app is still starting.
if (-not ('TaidaFlowStreamCollector' -as [type])) {
    Add-Type -TypeDefinition @'
public static class TaidaFlowStreamCollector {
    public static System.Text.StringBuilder Attach(System.IO.StreamReader reader) {
        System.Text.StringBuilder text = new System.Text.StringBuilder();
        System.Threading.Thread t = new System.Threading.Thread(delegate () {
            char[] buffer = new char[4096];
            int n;
            try { while ((n = reader.Read(buffer, 0, buffer.Length)) > 0) { lock (text) { text.Append(buffer, 0, n); } } } catch { }
        });
        t.IsBackground = true;
        t.Start();
        return text;
    }
    public static string Get(System.Text.StringBuilder text) { lock (text) { return text.ToString(); } }
}
'@
}
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $DataDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $appErr = [TaidaFlowStreamCollector]::Attach($p.StandardError)
    $appOut = [TaidaFlowStreamCollector]::Attach($p.StandardOutput)
} catch {
    Log "ERROR: TaidaFlowApp could not be started: $($_.Exception.Message)"
    if ($nginxOk) { Log "nginx is left running (independent of the app; stop it with: cd <nginx folder> ; nginx -s quit)" }
    exit 6
}
$state = [ordered]@{
    pid = $p.Id; exe = $exe; startTicksUtc = $p.StartTime.ToUniversalTime().Ticks; startTime = $p.StartTime.ToString('o')
    config = $env:TAIDAFLOW_CONFIG; dataDir = $DataDir; logDir = $LogDir; appLog = $fullLog; appQuietLog = $quietLog
    useNginx = $useNginxNow; nginxAvailable = $nginxOk; nginxStartedByThisScript = $nginxStartedNow
    nginxPort = $(if ($useNginxNow) { [int]$v['nginx.port'] } else { 0 }); downloadPort = (Get-TaidaFlowDownloadPort $cfg)
    httpPort = [int]$v['http.port']; mirrorPublicPort = [int]$v['mirror.publicPort']; mirrorInternalPort = [int]$v['mirror.internalPort']
    restPort = [int]$v['rest.port']; modbusServerPort = [int]$v['modbusServer.port']
}
[System.IO.File]::WriteAllText($stateFile, ($state | ConvertTo-Json), (New-Object System.Text.UTF8Encoding($false)))
Log "TaidaFlowApp started: pid $($p.Id) (no console window); its log files: $fullLog, $quietLog"

$appPorts = @([int]$v['modbusServer.port'], [int]$v['http.port'], [int]$v['mirror.publicPort'], [int]$v['mirror.internalPort'], [int]$v['rest.port'])
$t0 = Get-Date
$need = @([int]$v['http.port'], [int]$v['mirror.publicPort'])
$have = @()
do {
    Start-Sleep -Milliseconds 500
    $have = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
              Where-Object { $_.OwningProcess -eq $p.Id -and ($appPorts -contains $_.LocalPort) } |
              ForEach-Object { [int]$_.LocalPort } | Sort-Object -Unique)
    $missing = @($need | Where-Object { $have -notcontains $_ })
} while ($missing.Count -gt 0 -and -not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt $StartTimeoutSec)

# What the app wrote to stderr / stdout so far (normally nothing: its messages go to its own log files).
function Write-AppConsoleOutput {
    Start-Sleep -Milliseconds 300
    foreach ($s in @(@('stderr', $appErr), @('stdout', $appOut))) {
        $text = [TaidaFlowStreamCollector]::Get($s[1])
        $lines = @(($text -split "`r?`n") | Where-Object { $_.Trim() -ne '' })
        Log ("app {0} during start-up: {1} line(s){2}" -f $s[0], $lines.Count, $(if ($lines.Count -gt 40) { ' (last 40 below)' } else { '' }))
        foreach ($x in ($lines | Select-Object -Last 40)) { Log "  app $($s[0]) | $x" }
    }
}
if ($p.HasExited) {
    $code = $p.ExitCode
    Log "ERROR: TaidaFlowApp exited during start-up (exit code $code$(if ($code -eq 2) { ' = config.json unusable' }))"
    Write-AppConsoleOutput
    if ($code -eq 2) {
        # w2-064 A2: an unusable config.json is logged in <folder of that config.json>\logs.
        $fallback = Join-Path (Split-Path -Parent $env:TAIDAFLOW_CONFIG) 'logs'
        $fb = Join-Path $fallback "taidaflow-$today-full.log"
        Log "the app wrote the reason to its fallback log folder $fallback"
        if (Test-Path -LiteralPath $fb) { Get-Content -LiteralPath $fb -Tail 10 -Encoding UTF8 | ForEach-Object { Log "  app log | $_" } }
    } else {
        $last = if (Test-Path -LiteralPath $fullLog) { $fullLog } elseif (Test-Path -LiteralPath $quietLog) { $quietLog } else { $null }
        if ($last) { Log "last lines of $last :"; Get-Content -LiteralPath $last -Tail 15 -Encoding UTF8 | ForEach-Object { Log "  app log | $_" } }
        else { Log "no app log file in $LogDir (the app ended before it opened its log files)" }
    }
    Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
    if ($nginxOk) { Log "nginx is left running (independent of the app; stop it with: cd <nginx folder> ; nginx -s quit)" }
    exit 6
}
# Still running: whatever the app wrote to stderr / stdout so far (normally nothing). The pipes close
# when this script ends; later output of the app goes nowhere (its log files hold everything).
Write-AppConsoleOutput
foreach ($pp in $appPorts) {
    Log ("  port {0,-5} {1,-50} {2}" -f $pp, $portNames["$pp"], $(if ($have -contains $pp) { 'listening' } else { 'NOT listening' }))
}
if ($missing.Count -gt 0) {
    Log "ERROR: after $StartTimeoutSec s the app does not listen on $($missing -join ', ') - the app is left running (desktop HMI); see $fullLog"
    exit 6
}
if ($have -notcontains [int]$v['rest.port']) { Log "WARNING: the REST API does not listen on 127.0.0.1:$($v['rest.port']) (see [REST] in $fullLog); the app runs without it" }
$ips = @(try { Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop | Where-Object { $_.IPAddress -notlike '169.254.*' -and $_.IPAddress -ne '127.0.0.1' } | ForEach-Object { $_.IPAddress } } catch { })
foreach ($ip in @($ips) + @('127.0.0.1')) {
    if ($nginxOk) {
        $base = if ([int]$v['nginx.port'] -eq 80) { "http://${ip}" } else { "http://${ip}:$($v['nginx.port'])" }
        Log "web page: $base/  (nginx: page, /mirror, /exports, /api/; fallback http://${ip}:$($v['http.port'])/TaidaFlowApp.html)"
    }
    else { Log "web page: http://${ip}:$($v['http.port'])/TaidaFlowApp.html  (mirror ${ip}:$($v['mirror.publicPort']))" }
}
if ($useNginxNow -and -not $nginxOk) { Log "started WITHOUT nginx (exit 8)"; exit 8 }
Log "started (exit 0)"
exit 0

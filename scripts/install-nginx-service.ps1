# TaidaFlow - register the bundled nginx as the Windows service TaidaFlowNginx (WinSW).        (w2-076)
#
# FIELD machine (plant PC), run ONCE as ADMINISTRATOR from the installation folder (e.g. C:\TaidaFlow):
#     powershell -NoProfile -ExecutionPolicy Bypass -File C:\TaidaFlow\scripts\install-nginx-service.ps1 -WhatIf
#     (the same without -WhatIf, in a cmd / PowerShell started with "Run as administrator")
# -WhatIf needs no administrator and changes nothing: it prints the nginx-service.xml that would be written and
# every command that would be run.
#
# What it does (Mango 2026-09-29: "nginx registered as a service, starts automatically"):
#   1. reads config.json (<installation folder>\config.json or -Config): nginx.exe (default nginx\nginx.exe),
#      nginx.port, log.dir (resolved against dataDir, default C:\TaidaFlowData\logs);
#   2. makes sure <nginx folder>\conf\nginx.conf exists and belongs to this installation / config.json
#      (scripts\install-nginx-config.ps1 -IfChanged: written when missing or out of date) and that
#      "nginx -p <nginx folder> -t" passes - exactly the command line the service uses;
#   3. an nginx of this package started by hand / by start-taidaflow.ps1 ("start nginx") is stopped with
#      "nginx -p <nginx folder> -s quit" (waits up to -StopTimeoutSec); any OTHER program on nginx.port -> refused;
#   4. writes <installation folder>\nginx\nginx-service.xml for the wrapper nginx\nginx-service.exe (WinSW v2.12.0,
#      bundled by the package): service id TaidaFlowNginx, executable nginx.exe with -p <nginx folder>, stop =
#      nginx -p <nginx folder> -s quit, working directory = nginx folder, start mode Automatic, restart after a
#      crash, WinSW's own logs (nginx-service.wrapper.log / .out.log / .err.log) in the log folder;
#   5. not installed yet -> "nginx-service.exe install"; already installed from THIS folder -> the XML is
#      updated and the service restarted (running idempotently); installed from ANOTHER folder -> refused;
#   6. starts the service and waits until nginx listens on nginx.port.
# The service runs as LocalSystem (session 0, no window). start-taidaflow.ps1 recognises it (Win32_Service)
# and no longer starts a second nginx with "start nginx". TaidaFlowApp itself is NOT a service (it has the
# operator window): start it at log on with scripts\add-startup-shortcut.ps1 (Startup folder) or
# register-autostart.ps1 (Task Scheduler) - one of the two.
# Nothing is deleted. Remove the service with scripts\uninstall-nginx-service.ps1.
#
# Usage: install-nginx-service.ps1 [-WhatIf] [-Config <config.json>] [-StopTimeoutSec 60]
# Exit codes: 0 installed / updated and nginx listening (or -WhatIf printed); 2 missing file, config.json
# unusable, path not usable in the XML; 3 nginx.conf could not be written / "nginx -t" failed; 4 refused: the
# service TaidaFlowNginx belongs to another folder, or nginx.port is used by another program; 5 not an
# administrator (NOTHING changed); 6 service install / start failed or nginx not listening in time;
# 7 an nginx did not stop within -StopTimeoutSec (open web pages keep nginx busy - stop TaidaFlow first).
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$Config = "",
    [ValidateRange(5, 3600)]
    [int]$StopTimeoutSec = 60
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { [Console]::Out.WriteLine($m) }
. (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
$whatIf = [bool]$WhatIfPreference
# -WhatIf is handled by this script itself ($whatIf); cmdlets below (module auto-load, Get-FileHash, ...) must not
# see it, or they would skip their own read-only work.
$WhatIfPreference = $false

# --- administrator? (checked before anything else: without it nothing is read, written or started) --------
if (-not $whatIf -and -not (Test-TaidaFlowIsAdministrator)) {
    Say "REFUSED: install-nginx-service.ps1 registers the Windows service TaidaFlowNginx and needs an administrator."
    Say "  This PowerShell is not elevated - NOTHING was changed (no file written, no service, nothing started or stopped)."
    Say "  Start cmd with right-click > 'Run as administrator' and run the same command again,"
    Say "  or add -WhatIf to only print what would be done (no administrator needed)."
    exit 5
}

$install = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $install $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
$appExe = Join-Path $install 'TaidaFlowApp.exe'
$wrapper = Join-Path $install 'nginx\nginx-service.exe'
$xmlPath = Join-Path $install 'nginx\nginx-service.xml'
$confGen = Join-Path $install 'scripts\install-nginx-config.ps1'
foreach ($f in $appExe, $wrapper, $confGen) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { Say "missing: $f - incomplete package (nothing changed)"; exit 2 }
}
$configPath = if ($Config -ne "") { Full $Config } else { Join-Path $install 'config.json' }
$cfg = Get-TaidaFlowConfig -Path $configPath -Exe $appExe
if ($cfg.Error) { Say "config.json unusable: $($cfg.Error) (nothing changed)"; exit 2 }
$v = $cfg.Values
$nginxExe = Resolve-TaidaFlowNginxExe $cfg
$nginxDir = Split-Path -Parent $nginxExe
$logDir = Resolve-TaidaFlowLogDir $cfg
$port = [int]$v['nginx.port']
if (-not (Test-Path -LiteralPath $nginxExe -PathType Leaf)) { Say "nginx.exe not found: $nginxExe (config.json nginx.exe = $($v['nginx.exe'])) - nothing changed"; exit 2 }
$wrapperVer = (Get-Item -LiteralPath $wrapper).VersionInfo.ProductVersion
if ($wrapperVer) { $wrapperVer = ($wrapperVer -split '\+')[0] }
try {
    $xml = Get-TaidaFlowNginxServiceXml @{ NginxExe = $nginxExe; LogDir = $logDir; InstallDir = $install; ConfigPath = $configPath; Port = $port; WrapperVersion = $wrapperVer }
} catch { Say "$($_.Exception.Message) - nothing changed"; exit 2 }

$svc = Get-TaidaFlowNginxServiceInfo
if ($svc.Error) { Say "WARNING: $($svc.Error)" }
$mode = Get-TaidaFlowNginxServiceMode $svc $wrapper
$table = Get-TaidaFlowProcessTable
# nginx of this package started by hand / by start-taidaflow.ps1 (not by the service)
$manual = @(Get-Process nginx -ErrorAction SilentlyContinue | Where-Object {
    $pth = try { $_.Path } catch { '' }
    [string]::Equals($pth, $nginxExe, [System.StringComparison]::OrdinalIgnoreCase) -and
    -not (Test-TaidaFlowNginxServiceOwner -Service $svc -WrapperExe $wrapper -OwningPid $_.Id -Processes $table).Ours })
$manualIds = @($manual | ForEach-Object { $_.Id })
$foreign = New-Object System.Collections.Generic.List[string]
foreach ($l in @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)) {
    $lp = [int]$l.OwningProcess
    if ($manualIds -contains $lp) { continue }
    if ((Test-TaidaFlowNginxServiceOwner -Service $svc -WrapperExe $wrapper -OwningPid $lp -Processes $table).Ours) { continue }
    $owner = try { $pp = Get-Process -Id $lp -ErrorAction Stop; "$($pp.ProcessName) $($pp.Path)".Trim() } catch { '?' }
    if ($lp -eq 4) { $owner = 'System = Windows HTTP.sys (IIS, WinRM, WebDAV, a URL reservation ...; netsh http show servicestate)' }
    $foreign.Add("$($l.LocalAddress):$port pid $lp ($owner)")
}
$confFile = Join-Path $nginxDir 'conf\nginx.conf'
$oldXml = if (Test-Path -LiteralPath $xmlPath -PathType Leaf) { [System.IO.File]::ReadAllText($xmlPath) } else { $null }
$appRunning = @(Get-Process TaidaFlowApp -ErrorAction SilentlyContinue)

Say "=== TaidaFlow nginx service (WinSW) - $(if ($whatIf) { '-WhatIf: nothing is changed' } else { 'install' })"
Say "installation folder : $install"
Say "config.json         : $configPath$(if (-not $cfg.Exists) { ' (does not exist yet - default values; install-nginx-config.ps1 creates it)' })"
foreach ($n in $cfg.Notes) { Say "  config: $n" }
Say "nginx.exe           : $nginxExe (config.json nginx.exe = $($v['nginx.exe']), $($cfg.Sources['nginx.exe']))"
Say "nginx folder        : $nginxDir (prefix -p; conf\nginx.conf $(if (Test-Path -LiteralPath $confFile) { 'exists' } else { 'MISSING - will be written' }))"
Say "port                : $port (config.json nginx.port)$(if (-not $v['nginx.enabled']) { ' - NOTE: config.json nginx.enabled is false: the service runs nginx, but start-taidaflow.ps1 points the pages at the app''s own ports' })"
Say "log folder          : $logDir (config.json log.dir; WinSW writes nginx-service.wrapper.log / .out.log / .err.log there)"
Say "wrapper             : $wrapper (WinSW $wrapperVer, SHA-256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $wrapper).Hash))"
Say ("service {0}  : {1}" -f $script:TaidaFlowNginxServiceName, $(switch ($mode) { 'none' { 'not installed' } 'ours' { "installed from this folder, state $($svc.State), start mode $($svc.StartMode)" } default { "installed from ANOTHER folder: $($svc.PathName) (state $($svc.State))" } }))
Say "nginx started by hand: $(if ($manual.Count) { 'pid ' + ($manualIds -join ', ') + ' (will be stopped: nginx -s quit)' } else { 'none' })"
if ($foreign.Count) { foreach ($x in $foreign) { Say "port $port used by ANOTHER program: $x" } }
if ($appRunning.Count) { Say "NOTE: TaidaFlowApp is running (pid $(($appRunning | ForEach-Object { $_.Id }) -join ', ')): web pages with live synchronisation keep nginx busy while it stops - if a step below waits, stop TaidaFlow first (stop-taidaflow.bat)" }
Say ""
Say "nginx-service.xml ($xmlPath)$(if ($null -eq $oldXml) { ' - new' } elseif ($oldXml -ceq $xml) { ' - unchanged' } else { ' - will be REPLACED (the old one is kept as nginx-service.xml.prev-<time>)' }):"
foreach ($line in ($xml -split "`r?`n")) { if ($line -ne '') { Say "  | $line" } }
Say ""
Say "steps$(if ($whatIf) { ' (NOT executed: -WhatIf)' }):"
$q = '"'
$steps = New-Object System.Collections.Generic.List[string]
$steps.Add("powershell -NoProfile -ExecutionPolicy Bypass -File $q$confGen$q -Config $q$configPath$q -InstallDir $q$install$q -Nginx $q$nginxExe$q -IfChanged   (nginx.conf written when missing / out of date)")
$steps.Add("cd /d $q$nginxDir$q && ${q}nginx.exe$q -p $q$nginxDir$q -t   (must say: test is successful)")
if ($manual.Count) { $steps.Add("cd /d $q$nginxDir$q && ${q}nginx.exe$q -p $q$nginxDir$q -s quit   (stop the nginx started by hand, wait up to $StopTimeoutSec s)") }
$steps.Add("write $xmlPath (UTF-8, the text above)")
if ($mode -eq 'none') { $steps.Add("$q$wrapper$q install   (registers the service $($script:TaidaFlowNginxServiceName), start mode Automatic)") }
elseif ($mode -eq 'ours' -and $svc.State -ne 'Stopped') { $steps.Add("stop the service $($script:TaidaFlowNginxServiceName) (= $q$wrapper$q stop; wait up to $StopTimeoutSec s)") }
$steps.Add("start the service $($script:TaidaFlowNginxServiceName) (= $q$wrapper$q start / sc start $($script:TaidaFlowNginxServiceName))")
$steps.Add("check: nginx listens on port $port (an nginx.exe below the service process), $q$wrapper$q status = Started")
for ($i = 0; $i -lt $steps.Count; $i++) { Say ("  {0}. {1}" -f ($i + 1), $steps[$i]) }
Say ""

if ($mode -eq 'other') {
    Say "REFUSED: the service $($script:TaidaFlowNginxServiceName) is registered for another folder ($($svc.PathName))."
    Say "  Remove it first with the uninstall-nginx-service.ps1 of that folder, or (administrator cmd):"
    Say "    sc stop $($script:TaidaFlowNginxServiceName)"
    Say "    sc delete $($script:TaidaFlowNginxServiceName)"
    Say "  then run this script again. Nothing was changed."
    exit 4
}
if ($foreign.Count) {
    Say "REFUSED: port $port is used by another program (listed above; it is never stopped by this script)."
    Say "  Free the port or change config.json nginx.port (then this script again). Nothing was changed."
    exit 4
}
if ($whatIf) {
    Say "-WhatIf: NOTHING changed (no file written, no service installed / started / stopped, no nginx stopped)."
    exit 0
}

# --- 1. nginx.conf + nginx -t (the service's command line) -------------------------------------------------
Say "--- 1. nginx.conf"
$genOut = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $confGen -Config $configPath -InstallDir $install -Nginx $nginxExe -IfChanged
$genRc = $LASTEXITCODE
foreach ($line in @($genOut)) { if ("$line".Trim() -ne '') { Say "  install-nginx-config | $line" } }
if ($genRc -eq 4) { Say "  (exit 4 = port $port has a listener: nginx of this package / of the service, handled below)" }
elseif ($genRc -ne 0) { Say "nginx.conf could not be written / checked (install-nginx-config.ps1 exit $genRc) - service NOT installed"; exit 3 }
function Invoke-Nginx([string[]]$arguments, [int]$waitSec) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $nginxExe
    $psi.Arguments = (($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ')
    $psi.WorkingDirectory = $nginxDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $out = $p.StandardOutput.ReadToEnd()
    if (-not $p.WaitForExit($waitSec * 1000)) { return [pscustomobject]@{ rc = 'TIMEOUT'; out = $out } }
    return [pscustomobject]@{ rc = $p.ExitCode; out = ($out + $errTask.Result) }
}
$t = Invoke-Nginx @('-p', $nginxDir, '-t') 30
foreach ($line in ($t.out -split "`r?`n")) { if ($line.Trim() -ne '') { Say "  nginx -p -t | $line" } }
if ("$($t.rc)" -ne '0') { Say "nginx -p `"$nginxDir`" -t FAILED (exit $($t.rc)) - service NOT installed"; exit 3 }

# --- 2. stop an nginx of this package started by hand --------------------------------------------------------
if ($manual.Count) {
    Say "--- 2. stopping the nginx started by hand (pid $($manualIds -join ', ')): nginx -p `"$nginxDir`" -s quit"
    $manual | ForEach-Object { $null = $_.Handle }
    $qr = Invoke-Nginx @('-p', $nginxDir, '-s', 'quit') 30
    foreach ($line in ($qr.out -split "`r?`n")) { if ($line.Trim() -ne '') { Say "  nginx -s quit | $line" } }
    $deadline = (Get-Date).AddSeconds($StopTimeoutSec)
    while (@($manual | Where-Object { -not $_.HasExited }).Count -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    if (@($manual | Where-Object { -not $_.HasExited }).Count) {
        Say "the nginx started by hand did not stop within $StopTimeoutSec s (open web pages / downloads keep it busy) - service NOT installed."
        Say "  Stop TaidaFlow first (stop-taidaflow.bat) and run this script again (or: taskkill /F /IM nginx.exe as administrator)."
        exit 7
    }
    Say "  stopped"
}

# --- 3. nginx-service.xml ---------------------------------------------------------------------------------
Add-Type -AssemblyName System.ServiceProcess
Say "--- 3. $xmlPath"
if ($null -ne $oldXml -and $oldXml -cne $xml) {
    $backup = "$xmlPath.prev-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
    Copy-Item -LiteralPath $xmlPath -Destination $backup
    Say "  old file kept as $backup"
}
if ($null -eq $oldXml -or $oldXml -cne $xml) { [System.IO.File]::WriteAllText($xmlPath, $xml, (New-Object System.Text.UTF8Encoding($false))); Say "  written" }
else { Say "  unchanged" }
New-Item -ItemType Directory -Force $logDir | Out-Null

# --- 4. install, or stop for the restart ---------------------------------------------------------------------
# Runs nginx-service.exe <command> (native: its messages must not become PowerShell errors). Output lines;
# the exit code in $script:wrapperRc.
function Invoke-Wrapper([string]$command) {
    $ErrorActionPreference = 'Continue'
    $out = @(& $wrapper $command 2>&1 | ForEach-Object { "$_" })
    $script:wrapperRc = $LASTEXITCODE
    return $out
}
function Wait-Svc([string]$status, [int]$sec) {
    $sc = New-Object System.ServiceProcess.ServiceController $script:TaidaFlowNginxServiceName
    try { $sc.WaitForStatus($status, [TimeSpan]::FromSeconds($sec)); return $true } catch { return $false }
}
if ($mode -eq 'none') {
    Say "--- 4. $wrapper install"
    $o = Invoke-Wrapper install
    $rc = $script:wrapperRc
    foreach ($line in @($o)) { if ($line.Trim() -ne '') { Say "  nginx-service | $line" } }
    $svc = Get-TaidaFlowNginxServiceInfo
    if ($rc -ne 0 -or -not $svc.Exists) { Say "service installation FAILED (nginx-service.exe install exit $rc) - see $logDir\nginx-service.wrapper.log"; exit 6 }
    Say "  installed: $($svc.Name), start mode $($svc.StartMode), $($svc.PathName)"
} else {
    Say "--- 4. the service is already installed from this folder (state $($svc.State)): restart with the new XML"
    if ($svc.State -ne 'Stopped') {
        try { (New-Object System.ServiceProcess.ServiceController $script:TaidaFlowNginxServiceName).Stop() } catch { Say "  stop: $($_.Exception.Message)" }
        if (-not (Wait-Svc 'Stopped' $StopTimeoutSec)) {
            Say "the service did not stop within $StopTimeoutSec s (nginx -s quit waits for open web pages / downloads)."
            Say "  Stop TaidaFlow first (stop-taidaflow.bat) and run this script again; if it still hangs: taskkill /F /IM nginx.exe (administrator)."
            exit 7
        }
        Say "  stopped"
    }
}

# --- 5. start + check ---------------------------------------------------------------------------------------
Say "--- 5. starting the service $($script:TaidaFlowNginxServiceName)"
try { (New-Object System.ServiceProcess.ServiceController $script:TaidaFlowNginxServiceName).Start() } catch { Say "  start: $($_.Exception.Message)" }
if (-not (Wait-Svc 'Running' 30)) { Say "the service did not reach 'Running' within 30 s - see $logDir\nginx-service.wrapper.log and $logDir\nginx-error.log"; exit 6 }
$ok = $null
$t0 = Get-Date
do {
    Start-Sleep -Milliseconds 250
    $svc = Get-TaidaFlowNginxServiceInfo
    $table = Get-TaidaFlowProcessTable
    $ok = @(Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue |
            Where-Object { (Test-TaidaFlowNginxServiceOwner -Service $svc -WrapperExe $wrapper -OwningPid ([int]$_.OwningProcess) -Processes $table).Ours })
} while ($ok.Count -eq 0 -and ((Get-Date) - $t0).TotalSeconds -lt 20)
if ($ok.Count -eq 0) { Say "nginx of the service does not listen on port $port within 20 s - see $logDir\nginx-error.log, $logDir\nginx-service.wrapper.log"; exit 6 }
$st = Invoke-Wrapper status
Say "  nginx-service.exe status: $(($st | Where-Object { "$_".Trim() -ne '' }) -join ' | ')"
Say "  service $($svc.Name): state $($svc.State), start mode $($svc.StartMode), wrapper pid $($svc.ProcessId); nginx listening on $($ok[0].LocalAddress):$port (pid $($ok[0].OwningProcess))"
Say ""
Say "DONE: nginx runs as the Windows service $($svc.Name) and starts automatically with Windows."
Say "  start-taidaflow.bat recognises it (it no longer starts nginx itself). After a change of config.json run this script again"
Say "  (it regenerates nginx.conf and restarts the service). Remove the service: scripts\uninstall-nginx-service.ps1 (administrator)."
Say "  The operator program: scripts\add-startup-shortcut.ps1 (Startup folder) or register-autostart.ps1 (Task Scheduler) - one of the two."
exit 0

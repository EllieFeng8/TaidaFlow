# w2-076 unit test of the nginx-service decisions that deploy\release\start-taidaflow.ps1 uses (functions of
# scripts\taidaflow-config.ps1). No administrator, no service is installed.
#   A-E  pure functions with test data (TEST DATA ONLY: service / process tables written below):
#        PathName parsing, service mode, port-80 owner test (service of this package -> fine, another folder ->
#        refused, no service -> old behaviour), what to do with a regenerated nginx.conf, how to start nginx.
#   F    the owner test on REAL processes: a real nginx.exe (the package's bundled one) is started from THIS
#        PowerShell with a test prefix (build\w2-076-nginx-tree, listen 127.0.0.1:18976 only); a service record
#        whose process id is THIS PowerShell stands for the WinSW wrapper (the real parent of nginx). The real
#        Win32_Process table and the real listener are used. Then "nginx -s quit".
#   G    Win32_Service of a real LocalSystem service is readable by this normal user (PathName, ProcessId) while
#        Get-Process .Path of its process is not - the reason start-taidaflow.ps1 uses Win32_Service.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-076\tools\test-nginx-service-logic.ps1 -Package dist\TaidaFlow-<...>
# Exit 0 = all checks passed.
param([Parameter(Mandatory = $true)][string]$Package, [int]$TestPort = 18976)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
. (Join-Path $root 'scripts\taidaflow-config.ps1')
if (-not [System.IO.Path]::IsPathRooted($Package)) { $Package = Join-Path $root $Package }
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
$script:fail = 0; $script:n = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    $script:n++
    if (-not $ok) { $script:fail++ }
    [Console]::Out.WriteLine(("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' })))
}
[Console]::Out.WriteLine("=== w2-076 nginx service logic $((Get-Date).ToString('o')); admin=$(Test-TaidaFlowIsAdministrator)")

# --- A. PathName -> exe ----------------------------------------------------------------------------
Check 'A1 quoted PathName' ((ConvertFrom-TaidaFlowServicePathName '"C:\TaidaFlow\nginx\nginx-service.exe"') -eq 'C:\TaidaFlow\nginx\nginx-service.exe')
Check 'A2 quoted PathName with arguments' ((ConvertFrom-TaidaFlowServicePathName '"C:\Taida Flow\nginx\nginx-service.exe" /x') -eq 'C:\Taida Flow\nginx\nginx-service.exe')
Check 'A3 unquoted PathName with arguments' ((ConvertFrom-TaidaFlowServicePathName 'C:\Windows\system32\svchost.exe -k netsvcs -p') -eq 'C:\Windows\system32\svchost.exe')
Check 'A4 empty PathName' ((ConvertFrom-TaidaFlowServicePathName '') -eq '')

# --- test data -----------------------------------------------------------------------------------------
$wrapper = 'C:\TaidaFlow\nginx\nginx-service.exe'
function Svc([bool]$exists, [string]$path, [string]$state, [int]$svcPid) {
    return [pscustomobject]@{ Name = 'TaidaFlowNginx'; Exists = $exists; State = $state; StartMode = 'Auto'; PathName = $(if ($path) { '"' + $path + '"' } else { '' })
                              ExePath = $(if ($path) { $path } else { '' }); ProcessId = $svcPid; Error = '' }
}
$ours = Svc $true 'C:\TaidaFlow\nginx\nginx-service.exe' 'Running' 1000
$oursUpper = Svc $true 'C:\TAIDAFLOW\NGINX\NGINX-SERVICE.EXE' 'Running' 1000
$other = Svc $true 'D:\OldTaidaFlow\nginx\nginx-service.exe' 'Running' 1000
$none = Svc $false '' '' 0
$stopped = Svc $true 'C:\TaidaFlow\nginx\nginx-service.exe' 'Stopped' 0
# services.exe 700 -> wrapper 1000 -> nginx master 2000 -> nginx worker 2001 ; unrelated nginx 2100 (explorer 5000);
# httpd 3000 below the wrapper; System pid 4
$procs = @{
    700 = @{ Name = 'services.exe'; ParentProcessId = 600 }; 1000 = @{ Name = 'nginx-service.exe'; ParentProcessId = 700 }
    2000 = @{ Name = 'nginx.exe'; ParentProcessId = 1000 }; 2001 = @{ Name = 'nginx.exe'; ParentProcessId = 2000 }
    5000 = @{ Name = 'explorer.exe'; ParentProcessId = 4900 }; 2100 = @{ Name = 'nginx.exe'; ParentProcessId = 5000 }
    3000 = @{ Name = 'httpd.exe'; ParentProcessId = 1000 }; 4 = @{ Name = 'System'; ParentProcessId = 0 }
}

# --- B. service mode -------------------------------------------------------------------------------------
Check 'B1 no service -> none' ((Get-TaidaFlowNginxServiceMode $none $wrapper) -eq 'none')
Check 'B2 PathName = this package wrapper -> ours' ((Get-TaidaFlowNginxServiceMode $ours $wrapper) -eq 'ours')
Check 'B3 same path, other letter case -> ours' ((Get-TaidaFlowNginxServiceMode $oursUpper $wrapper) -eq 'ours')
Check 'B4 PathName of another folder -> other' ((Get-TaidaFlowNginxServiceMode $other $wrapper) -eq 'other')

# --- C. owner of the port 80 listener --------------------------------------------------------------------
function Own($s, [int]$ownerPid) { return Test-TaidaFlowNginxServiceOwner -Service $s -WrapperExe $wrapper -OwningPid $ownerPid -Processes $procs }
$r = Own $ours 2001; Check 'C1 service of this package, listener = nginx worker below the wrapper -> fine (not "port in use")' ($r.Ours) $r.Reason
$r = Own $ours 2000; Check 'C2 service of this package, listener = nginx master below the wrapper -> fine' ($r.Ours) $r.Reason
$r = Own $other 2001; Check 'C3 service PathName points to ANOTHER folder -> refused (not ours)' (-not $r.Ours) $r.Reason
$r = Own $none 2001; Check 'C4 no service -> not ours (start-taidaflow keeps its old path check)' (-not $r.Ours) $r.Reason
$r = Own $stopped 2001; Check 'C5 service stopped -> not ours' (-not $r.Ours) $r.Reason
$r = Own $ours 3000; Check 'C6 listener is not nginx.exe (httpd below the wrapper) -> not ours' (-not $r.Ours) $r.Reason
$r = Own $ours 2100; Check 'C7 an nginx.exe NOT below the service (started from explorer) -> not ours' (-not $r.Ours) $r.Reason
$r = Own $ours 4; Check 'C8 pid 4 System (HTTP.sys) -> not ours' (-not $r.Ours) $r.Reason
$r = Own $ours 9999; Check 'C9 unknown pid -> not ours' (-not $r.Ours) $r.Reason

# --- D. regenerated nginx.conf: what start-taidaflow does ------------------------------------------------
#           written  mode   state      admin   manualRunning  expected
$dCases = @(
    @($true,  'none',  '',        $false, $true,  'reload',          'D1 no service, nginx started by hand runs -> nginx -s reload (as before)'),
    @($true,  'none',  '',        $false, $false, 'none',            'D2 no service, no nginx running -> nothing (it starts with the new file)'),
    @($false, 'none',  '',        $false, $true,  'none',            'D3 file unchanged -> nothing'),
    @($true,  'ours',  'Running', $false, $false, 'tell-admin',      'D4 OUR service runs, NOT administrator -> no reload, message (exit 3)'),
    @($true,  'ours',  'Running', $false, $true,  'tell-admin',      'D5 OUR service runs, not admin, even if nginx paths look "manual" -> no reload'),
    @($true,  'ours',  'Running', $true,  $false, 'restart-service', 'D6 OUR service runs, administrator -> restart the service'),
    @($true,  'ours',  'Stopped', $false, $false, 'none',            'D7 OUR service stopped -> nothing (it reads the new file when started)'),
    @($true,  'other', 'Running', $false, $true,  'reload',          'D8 service of ANOTHER folder is ignored -> old behaviour (reload own nginx)'))
foreach ($c in $dCases) {
    $a = Get-TaidaFlowNginxConfAction $c[0] $c[1] $c[2] $c[3] $c[4]
    Check $c[6] ($a -eq $c[5]) "got $a"
}

# --- E. how nginx is started ----------------------------------------------------------------------------
$eCases = @(
    @('none',  '',              $true,  'leave',         'E1 no service, nginx running -> left as it is (as before)'),
    @('none',  '',              $false, 'start-process', 'E2 no service, nginx not running -> start nginx (as before)'),
    @('ours',  'Running',       $false, 'leave',         'E3 OUR service running -> left as it is, NO second "start nginx"'),
    @('ours',  'Running',       $true,  'leave',         'E4 OUR service running (admin sees the paths) -> left as it is'),
    @('ours',  'Start Pending', $false, 'leave',         'E5 OUR service starting -> wait, no "start nginx"'),
    @('ours',  'Stopped',       $false, 'start-service', 'E6 OUR service stopped -> start the SERVICE, not "start nginx"'),
    @('other', 'Running',       $false, 'start-process', 'E7 service of another folder -> old behaviour (port check decides)'))
foreach ($c in $eCases) {
    $a = Get-TaidaFlowNginxStartAction $c[0] $c[1] $c[2]
    Check $c[4] ($a -eq $c[3]) "got $a"
}

# --- F. owner test on real processes ----------------------------------------------------------------------
$nginxExe = Join-Path $Package 'nginx\nginx.exe'
$prefix = Join-Path $root 'build\w2-076-nginx-tree'
if (-not (Test-Path $nginxExe)) { Check "F0 package nginx.exe exists" $false $nginxExe }
elseif (@(Get-NetTCPConnection -State Listen -LocalPort $TestPort -ErrorAction SilentlyContinue).Count) { Check "F0 test port $TestPort free" $false }
else {
    if (Test-Path $prefix) { Remove-Item -LiteralPath $prefix -Recurse -Force }
    New-Item -ItemType Directory -Force (Join-Path $prefix 'conf'), (Join-Path $prefix 'logs'), (Join-Path $prefix 'temp') | Out-Null
    Copy-Item -LiteralPath (Join-Path $Package 'nginx\conf\mime.types') -Destination (Join-Path $prefix 'conf')
    $conf = "worker_processes 1;`nerror_log logs/error.log warn;`npid logs/nginx.pid;`nevents { worker_connections 64; }`n" +
            "http { access_log off; server { listen 127.0.0.1:$TestPort; location / { return 204; } } }`n"
    [System.IO.File]::WriteAllText((Join-Path $prefix 'conf\nginx.conf'), $conf, (New-Object System.Text.UTF8Encoding($false)))
    $np = Start-Process -FilePath $nginxExe -ArgumentList @('-p', ('"' + $prefix + '"')) -WorkingDirectory $prefix -WindowStyle Hidden -PassThru
    $null = $np.Handle
    $l = @()
    $t0 = Get-Date
    do { Start-Sleep -Milliseconds 250; $l = @(Get-NetTCPConnection -State Listen -LocalPort $TestPort -ErrorAction SilentlyContinue) } while ($l.Count -eq 0 -and ((Get-Date) - $t0).TotalSeconds -lt 15)
    Check "F1 real nginx (package nginx.exe, -p test prefix) listens on 127.0.0.1:$TestPort" ($l.Count -gt 0) "master pid $($np.Id)"
    if ($l.Count) {
        $table = Get-TaidaFlowProcessTable
        $ownerPid = [int]$l[0].OwningProcess
        [Console]::Out.WriteLine("     listener owner pid $ownerPid ($($table[$ownerPid].Name)), parent $($table[$ownerPid].ParentProcessId); this PowerShell pid $PID")
        $asWrapper = [pscustomobject]@{ Name = 'TaidaFlowNginx'; Exists = $true; State = 'Running'; StartMode = 'Auto'; PathName = '"' + $wrapper + '"'; ExePath = $wrapper; ProcessId = $PID; Error = '' }
        $r = Test-TaidaFlowNginxServiceOwner -Service $asWrapper -WrapperExe $wrapper -OwningPid $ownerPid -Processes $table
        Check 'F2 real process tree: the listener is an nginx.exe below the "wrapper" process (this PowerShell) -> ours' ($r.Ours) $r.Reason
        $explorer = @(Get-Process explorer -ErrorAction SilentlyContinue | Select-Object -First 1)
        $otherPid = if ($explorer.Count) { $explorer[0].Id } else { 4 }
        $asOther = [pscustomobject]@{ Name = 'TaidaFlowNginx'; Exists = $true; State = 'Running'; StartMode = 'Auto'; PathName = '"' + $wrapper + '"'; ExePath = $wrapper; ProcessId = $otherPid; Error = '' }
        $r = Test-TaidaFlowNginxServiceOwner -Service $asOther -WrapperExe $wrapper -OwningPid $ownerPid -Processes $table
        Check "F3 real process tree: service pid = another process ($otherPid) -> not ours" (-not $r.Ours) $r.Reason
        $asOtherFolder = [pscustomobject]@{ Name = 'TaidaFlowNginx'; Exists = $true; State = 'Running'; StartMode = 'Auto'; PathName = '"D:\x\nginx\nginx-service.exe"'; ExePath = 'D:\x\nginx\nginx-service.exe'; ProcessId = $PID; Error = '' }
        $r = Test-TaidaFlowNginxServiceOwner -Service $asOtherFolder -WrapperExe $wrapper -OwningPid $ownerPid -Processes $table
        Check 'F4 real process tree, service PathName of another folder -> not ours' (-not $r.Ours) $r.Reason
    }
    $q = Start-Process -FilePath $nginxExe -ArgumentList @('-p', ('"' + $prefix + '"'), '-s', 'quit') -WorkingDirectory $prefix -WindowStyle Hidden -PassThru -Wait
    $deadline = (Get-Date).AddSeconds(15)
    while (-not $np.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
    Start-Sleep -Milliseconds 500
    Check "F5 nginx -p <prefix> -s quit stops it, port $TestPort free again" ($np.HasExited -and @(Get-NetTCPConnection -State Listen -LocalPort $TestPort -ErrorAction SilentlyContinue).Count -eq 0) "quit exit $($q.ExitCode)"
    Remove-Item -LiteralPath $prefix -Recurse -Force -ErrorAction SilentlyContinue
}

# --- G. Win32_Service readable by a normal user --------------------------------------------------------
$real = Get-TaidaFlowNginxServiceInfo 'Schedule'
$pathViaProcess = if ($real.ProcessId -gt 0) { try { (Get-Process -Id $real.ProcessId -ErrorAction Stop).Path } catch { '' } } else { '' }
Check 'G1 Win32_Service of a LocalSystem service (Schedule) readable without administrator: PathName + ProcessId' ($real.Exists -and $real.PathName -ne '' -and $real.ProcessId -gt 0) "PathName $($real.PathName), pid $($real.ProcessId), Get-Process .Path = '$pathViaProcess'"
$tf = Get-TaidaFlowNginxServiceInfo
Check 'G2 TaidaFlowNginx is not installed on this development PC (no service was installed by this task)' (-not $tf.Exists -and -not $tf.Error) "Exists=$($tf.Exists) $($tf.Error)"

[Console]::Out.WriteLine("=== $($script:n) check(s), $($script:fail) failed")
if ($script:fail) { exit 1 }
exit 0

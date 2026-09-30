# w2-076: checks a generated nginx-service.xml against WinSW v2.12.0 (no administrator, nothing installed).
#   1. well-formed XML, root <service>;
#   2. every element name is one WinSW v2.12.0 reads (list below = doc/xmlConfigFile.md + loggingAndErrorReporting
#      of the v2.12.0 tag and src/WinSW.Core/Configuration/XmlServiceConfig.cs), and the documented rules:
#      <startarguments> (not <arguments>) together with <stoparguments>; startmode is a ServiceStartMode name;
#      onfailure action restart|none|reboot; the securityDescriptor SDDL is accepted by
#      System.Security.AccessControl.RawSecurityDescriptor (the class WinSW's install uses);
#   3. the values expected for TaidaFlow: id TaidaFlowNginx, nginx.exe with -p <its folder>, stop "-s quit",
#      working directory = nginx folder, Automatic, logpath given;
#   4. the REAL WinSW parses it: a copy of the wrapper next to the XML runs "status" (read-only: it loads the
#      XML, then asks the service manager; no elevation, nothing installed) and must print "NonExistent".
# Usage: check-winsw-xml.ps1 -Xml <nginx-service.xml> -WinSW <WinSW exe> -WorkDir <empty test folder>
param([Parameter(Mandatory = $true)][string]$Xml, [Parameter(Mandatory = $true)][string]$WinSW, [Parameter(Mandatory = $true)][string]$WorkDir)
$ErrorActionPreference = 'Stop'
$script:fail = 0; $script:n = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
    $script:n++; if (-not $ok) { $script:fail++ }
    [Console]::Out.WriteLine(("[{0}] {1}{2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what, $(if ($detail) { " - $detail" } else { '' })))
}
$doc = New-Object System.Xml.XmlDocument
try { $doc.Load($Xml); Check 'X1 well-formed XML' $true $Xml } catch { Check 'X1 well-formed XML' $false $_.Exception.Message; exit 1 }
Check 'X2 root element <service>' ($doc.DocumentElement.PSBase.Name -eq 'service') $doc.DocumentElement.PSBase.Name
$known = @('id', 'name', 'description', 'executable', 'startmode', 'delayedAutoStart', 'depend', 'logpath', 'log', 'logmode', 'arguments', 'argument',
           'startarguments', 'startargument', 'stopexecutable', 'stoparguments', 'stopargument', 'stoptimeout', 'env', 'interactive', 'beeponshutdown',
           'download', 'onfailure', 'resetfailure', 'securityDescriptor', 'serviceaccount', 'workingdirectory', 'priority', 'stopparentprocessfirst',
           'hidewindow', 'waithint', 'sleeptime', 'extensions', 'outfiledisabled', 'errfiledisabled', 'logname')
$logChildren = @('sizeThreshold', 'keepFiles', 'pattern', 'autoRollAtTime', 'zipOlderThanNumDays', 'zipDateFormat', 'period')
$top = @($doc.DocumentElement.ChildNodes | Where-Object { $_.NodeType -eq 'Element' })
foreach ($e in $top) {
    Check "X3 <$($e.PSBase.Name)> is a WinSW v2.12.0 element" ($known -ccontains $e.PSBase.Name) ("value: " + $(if ($e.PSBase.Name -eq 'log') { "mode=$($e.GetAttribute('mode'))" } elseif ($e.PSBase.Name -eq 'onfailure') { "action=$($e.GetAttribute('action')) delay=$($e.GetAttribute('delay'))" } else { $e.InnerText }))
}
$log = $doc.SelectSingleNode('/service/log')
if ($log) {
    Check 'X4 <log mode> is a WinSW mode' (@('append', 'reset', 'ignore', 'roll', 'roll-by-time', 'roll-by-size', 'roll-by-size-time', 'none') -contains $log.GetAttribute('mode')) $log.GetAttribute('mode')
    foreach ($c in @($log.ChildNodes | Where-Object { $_.NodeType -eq 'Element' })) { Check "X5 <log><$($c.PSBase.Name)> is a WinSW log setting (integer)" ($logChildren -ccontains $c.PSBase.Name -and $c.InnerText -match '^\d+$') $c.InnerText }
}
function T([string]$xp) { $x = $doc.SelectSingleNode($xp); if ($x) { return $x.InnerText } return $null }
Check 'X6 <stoparguments> is used with <startarguments>, no <arguments> (WinSW appends <arguments> to the stop command)' ((T '/service/stoparguments') -and (T '/service/startarguments') -and -not (T '/service/arguments')) ''
Add-Type -AssemblyName System.ServiceProcess
$sm = T '/service/startmode'
Check 'X7 <startmode> is a ServiceStartMode name (Boot, System, Automatic, Manual, Disabled)' ([enum]::GetNames([System.ServiceProcess.ServiceStartMode]) -contains $sm) $sm
foreach ($f in @($doc.SelectNodes('/service/onfailure'))) { Check 'X8 <onfailure action> restart|none|reboot' (@('restart', 'none', 'reboot') -contains $f.GetAttribute('action')) $f.GetAttribute('action') }
$sddl = T '/service/securityDescriptor'
if ($sddl) {
    try { $raw = New-Object System.Security.AccessControl.RawSecurityDescriptor($sddl); $ok = $true } catch { $ok = $false; $raw = $null }
    Check 'X9 <securityDescriptor> accepted by RawSecurityDescriptor (as WinSW install does)' $ok $sddl
    if ($raw) {
        foreach ($ace in $raw.DiscretionaryAcl) {
            $who = try { $ace.SecurityIdentifier.Translate([System.Security.Principal.NTAccount]).Value } catch { $ace.SecurityIdentifier.Value }
            [Console]::Out.WriteLine(("     ACE {0,-40} access mask 0x{1:X}" -f $who, $ace.AccessMask))
        }
        $iu = @($raw.DiscretionaryAcl | Where-Object { $_.SecurityIdentifier.Value -eq 'S-1-5-4' })
        # SERVICE_START 0x10, SERVICE_STOP 0x20, SERVICE_CHANGE_CONFIG 0x2, DELETE 0x10000
        Check 'X10 INTERACTIVE users may start (0x10) but not stop (0x20), change (0x2) or delete (0x10000) the service' ($iu.Count -eq 1 -and ($iu[0].AccessMask -band 0x10) -and -not ($iu[0].AccessMask -band 0x20) -and -not ($iu[0].AccessMask -band 0x2) -and -not ($iu[0].AccessMask -band 0x10000)) ("0x{0:X}" -f $(if ($iu.Count) { $iu[0].AccessMask } else { 0 }))
    }
}
# TaidaFlow values
$exe = T '/service/executable'
$dir = if ($exe) { Split-Path -Parent $exe } else { '' }
Check 'X11 <id> TaidaFlowNginx' ((T '/service/id') -ceq 'TaidaFlowNginx') (T '/service/id')
Check 'X12 <executable> = nginx.exe (full path)' ($exe -and [System.IO.Path]::IsPathRooted($exe) -and (Split-Path -Leaf $exe) -eq 'nginx.exe') $exe
Check 'X13 <startarguments> = -p "<nginx folder>"' ((T '/service/startarguments') -ceq ('-p "' + $dir + '"')) (T '/service/startarguments')
Check 'X14 <stopexecutable> = the same nginx.exe, <stoparguments> = -p "<nginx folder>" -s quit' ((T '/service/stopexecutable') -ceq $exe -and (T '/service/stoparguments') -ceq ('-p "' + $dir + '" -s quit')) (T '/service/stoparguments')
Check 'X15 <workingdirectory> = nginx folder' ((T '/service/workingdirectory') -ceq $dir) (T '/service/workingdirectory')
Check 'X16 <startmode> Automatic' ($sm -ceq 'Automatic') $sm
Check 'X17 <logpath> is an absolute folder (config.json log.dir)' ((T '/service/logpath') -and [System.IO.Path]::IsPathRooted((T '/service/logpath'))) (T '/service/logpath')
Check 'X18 no % in any value (WinSW would expand %VAR%)' (-not ($doc.OuterXml -match '%')) ''

# the real WinSW
if (Test-Path $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$exeCopy = Join-Path $WorkDir 'nginx-service.exe'
Copy-Item -LiteralPath $WinSW -Destination $exeCopy
# the XML's logpath would be the plant log folder: the copy used here points it to the test folder
$doc2 = New-Object System.Xml.XmlDocument; $doc2.Load($Xml)
$doc2.SelectSingleNode('/service/logpath').InnerText = (Join-Path $WorkDir 'logs')
$doc2.Save((Join-Path $WorkDir 'nginx-service.xml'))
$ErrorActionPreference = 'Continue'
$out = @(& $exeCopy status 2>&1 | ForEach-Object { "$_" })
$rc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
[Console]::Out.WriteLine("     nginx-service.exe status (copy in $WorkDir, logpath -> $WorkDir\logs): exit $rc : $($out -join ' | ')")
Check 'X19 the real WinSW 2.12.0 loads the XML and answers "status" = NonExistent (nothing installed)' ($rc -eq 0 -and ($out -join ' ') -match 'NonExistent') "exit $rc"
$wl = Join-Path $WorkDir 'logs\nginx-service.wrapper.log'
if (Test-Path $wl) { Get-Content -LiteralPath $wl | ForEach-Object { [Console]::Out.WriteLine("     wrapper.log | $_") } }
$ErrorActionPreference = 'Continue'
$vout = @(& $exeCopy version 2>&1 | ForEach-Object { "$_" }); $vrc = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Check 'X20 WinSW version = 2.12.0' ($vrc -eq 0 -and ($vout -join ' ') -match '2\.12\.0') ($vout -join ' | ')
$svcNow = @(Get-CimInstance Win32_Service -Filter "Name='TaidaFlowNginx'").Count
Check 'X21 still no service TaidaFlowNginx' ($svcNow -eq 0) ''
Remove-Item -LiteralPath $WorkDir -Recurse -Force
[Console]::Out.WriteLine("=== $($script:n) check(s), $($script:fail) failed")
if ($script:fail) { exit 1 }
exit 0

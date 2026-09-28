# TaidaFlow config.json reader for the PowerShell scripts (w2-062, docs/taidaflow_config_spec.md).
# Dot-source it:   . (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
# Used by: run-desktop.ps1, safety_probe.ps1, nginx-web.ps1, deploy-web.ps1, verify-*.ps1 (repository)
# and start-/stop-taidaflow.ps1, register-autostart.ps1 (package: <package>\scripts\taidaflow-config.ps1).
#
# The application itself (App/appconfig.cpp) is the ONLY place that defines the default values:
#   * a missing config.json is created with   TaidaFlowApp.exe --write-default-config <path>
#     (Invoke-TaidaFlowWriteDefaultConfig);
#   * a key that is missing / invalid in the file takes the default from a temporary file written the
#     same way (Get-TaidaFlowDefaultConfig) - no default value is repeated in any script.
# The validation rules below are the ones of AppConfig (type and range per key): a value the app
# would reject (-> default + warning) is rejected here the same way, so scripts and app agree.
# w2-065: "nginx": {"exe": ...} and "log": {...} are keys of the app (w2-064, App/appconfig.cpp);
# their defaults come from --write-default-config like every other key (the w2-062 script fallback
# "nginx\nginx.exe" is removed). Paths are resolved exactly like the app does:
#   nginx.exe : relative -> folder of config.json (= installation folder), absolute as it is
#               (AppConfig::resolvedNginxExe); Resolve-TaidaFlowNginxExe. -Nginx of a script overrides it.
#   log.dir   : relative -> the resolved dataDir (default "logs" -> C:\TaidaFlowData\logs), absolute as
#               it is (AppConfig::resolvedLogDir); Resolve-TaidaFlowLogDir. The app writes its own
#               taidaflow-YYYY-MM-DD.log / -full.log there; the scripts add launcher-YYYY-MM-DD.log and
#               the generated nginx.conf writes nginx-access-YYYY-MM-DD.log / nginx-error.log there.
#
# Functions
#   Resolve-TaidaFlowConfigPath <root> [<explicit>]   which config.json a script uses:
#       1. the explicit path (-Config); 2. the environment variable TAIDAFLOW_CONFIG;
#       3. <root>\deploy\dev\config.dev.json when it exists (development repository);
#       4. <root>\config.json (installed package: next to TaidaFlowApp.exe).
#   Get-TaidaFlowConfig -Path <file> -Exe <TaidaFlowApp.exe> [-Create]
#       -> object: Path, Exists, Created, Error ('' = usable), Values (key -> value, keys like
#          "devices.adam6256.host"), Sources (key -> file|default|override), Notes (list), DataDir
#          (resolved full path), BaseDir (folder of the file). -Create writes the default file first
#          when it is missing. JSON error -> Error set, file untouched (the app exits with code 2).
#   Set-TaidaFlowConfigValue $cfg <key> <value>       one-time override (source "override")
#   Write-TaidaFlowConfigFile $cfg <path>             complete config.json of the effective values
#                                                     (dataDir written as a full path)
#   Get-TaidaFlowRuntimeJson <port>                   {"mirrorPublicPort":<port>,"version":1} - the
#                                                     same bytes as TaidaFlowRuntime::buildRuntimeJson
#   Write-TaidaFlowRuntimeJson <web folder> <port>    writes <web>\runtime.json (only when changed),
#                                                     removes a runtime.json.gz; returns a message
#   Get-TaidaFlowDownloadPort $cfg                    nginx.enabled ? nginx.port : http.port
#   Get-TaidaFlowPagePort $cfg                        nginx.enabled ? nginx.port : mirror.publicPort (runtime.json)
#   Resolve-TaidaFlowNginxExe $cfg                    full path of nginx.exe (relative = to the config folder)
#   Resolve-TaidaFlowLogDir $cfg                      full path of log.dir (relative = to the resolved dataDir)
#   Remove-TaidaFlowExpiredDatedFiles <dir> <prefix> <suffix> <keepDays>
#                                                     deletes <prefix>YYYY-MM-DD<suffix> files older than
#                                                     keepDays days (today included); nothing else
#   Get-TaidaFlowNginxConf <template> <values>        the complete TaidaFlow nginx configuration text

$script:TaidaFlowDefaultCache = @{}

function Get-TaidaFlowKeyKind([string]$key) {
    switch -Regex ($key) {
        '^version$'                              { return @{ kind = 'int'; min = 1; max = 1 } }
        '^dataDir$'                              { return @{ kind = 'text' } }
        '^devices\.ms300\.serialPort$'           { return @{ kind = 'text' } }
        '^devices\.ms300\.baudRate$'             { return @{ kind = 'intchoice'; choices = @(1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200) } }
        '^devices\.ms300\.dataBits$'             { return @{ kind = 'int'; min = 5; max = 8 } }
        '^devices\.ms300\.parity$'               { return @{ kind = 'choice'; choices = @('none', 'even', 'odd', 'space', 'mark') } }
        '^devices\.ms300\.stopBits$'             { return @{ kind = 'intchoice'; choices = @(1, 2) } }
        '^devices\.ms300\.unitId$'               { return @{ kind = 'int'; min = 1; max = 247 } }
        '^devices\.[a-z0-9]+\.host$'             { return @{ kind = 'address' } }
        '\.(port|internalPort|publicPort)$'      { return @{ kind = 'port' } }
        '\.unitId$'                              { return @{ kind = 'int'; min = 0; max = 255 } }
        '\.(bind|publicBind)$'                   { return @{ kind = 'address' } }
        '^nginx\.enabled$'                       { return @{ kind = 'bool' } }
        '^nginx\.exe$'                           { return @{ kind = 'text' } }
        # w2-064 log keys (App/appconfig.cpp: Text, Bool, Integer 1..INT_MAX)
        '^log\.dir$'                             { return @{ kind = 'text' } }
        '^log\.(quiet|full)\.enabled$'           { return @{ kind = 'bool' } }
        '^log\.(quiet|full)\.keepDays$'          { return @{ kind = 'int'; min = 1; max = 2147483647 } }
    }
    return @{ kind = 'unknown' }
}

function Test-TaidaFlowWholeNumber($v, [int64]$min, [int64]$max) {
    if ($v -is [bool] -or $null -eq $v) { return $null }
    if ($v -is [int] -or $v -is [long] -or $v -is [decimal] -or $v -is [double] -or $v -is [single]) {
        $d = [double]$v
        if ([double]::IsNaN($d) -or [double]::IsInfinity($d) -or [math]::Floor($d) -ne $d -or $d -lt $min -or $d -gt $max) { return $null }
        return [int64]$d
    }
    return $null
}

# Returns @{ ok; value; reason } for one key.
function Test-TaidaFlowValue([string]$key, $value) {
    $k = Get-TaidaFlowKeyKind $key
    switch ($k.kind) {
        'int' {
            $n = Test-TaidaFlowWholeNumber $value $k.min $k.max
            if ($null -ne $n) { return @{ ok = $true; value = [int]$n } }
            return @{ ok = $false; reason = "expected an integer $($k.min)..$($k.max)" }
        }
        'port' {
            $n = Test-TaidaFlowWholeNumber $value 1 65535
            if ($null -ne $n) { return @{ ok = $true; value = [int]$n } }
            return @{ ok = $false; reason = 'expected a port number 1..65535' }
        }
        'intchoice' {
            $n = Test-TaidaFlowWholeNumber $value ([int]::MinValue) ([int]::MaxValue)
            if ($null -ne $n -and $k.choices -contains [int]$n) { return @{ ok = $true; value = [int]$n } }
            return @{ ok = $false; reason = "expected one of: $($k.choices -join ', ')" }
        }
        'address' {
            if ($value -is [string]) {
                $t = $value.Trim(); $ip = $null
                $shapeOk = ($t -match '^\d{1,3}(\.\d{1,3}){3}$') -or ($t.Contains(':'))
                if ($t -ne '' -and $shapeOk -and [System.Net.IPAddress]::TryParse($t, [ref]$ip)) { return @{ ok = $true; value = $t } }
            }
            return @{ ok = $false; reason = 'expected an IP address such as "192.168.1.201"' }
        }
        'text' {
            if ($value -is [string] -and $value.Trim() -ne '') { return @{ ok = $true; value = $value.Trim() } }
            return @{ ok = $false; reason = 'expected a non-empty string' }
        }
        'bool' {
            if ($value -is [bool]) { return @{ ok = $true; value = $value } }
            return @{ ok = $false; reason = 'expected true or false' }
        }
        'choice' {
            if ($value -is [string] -and $k.choices -contains $value.Trim().ToLowerInvariant()) { return @{ ok = $true; value = $value.Trim().ToLowerInvariant() } }
            return @{ ok = $false; reason = "expected one of: $($k.choices -join ', ')" }
        }
    }
    return @{ ok = $false; reason = 'unsupported setting' }
}

function Resolve-TaidaFlowConfigPath([string]$root, [string]$explicit = '') {
    if ($explicit -ne '') {
        if (-not [System.IO.Path]::IsPathRooted($explicit)) { $explicit = Join-Path (Get-Location).Path $explicit }
        return [System.IO.Path]::GetFullPath($explicit)
    }
    if ($env:TAIDAFLOW_CONFIG -and $env:TAIDAFLOW_CONFIG.Trim() -ne '') { return [System.IO.Path]::GetFullPath($env:TAIDAFLOW_CONFIG.Trim()) }
    $dev = Join-Path $root 'deploy\dev\config.dev.json'
    if (Test-Path -LiteralPath $dev -PathType Leaf) { return [System.IO.Path]::GetFullPath($dev) }
    return [System.IO.Path]::GetFullPath((Join-Path $root 'config.json'))
}

# Runs TaidaFlowApp.exe --write-default-config <path>. Returns @{ rc; output }.
# rc: 0 written, 3 exists (never overwritten), 1 not writable, 4 no path, other = could not run.
function Invoke-TaidaFlowWriteDefaultConfig([string]$Exe, [string]$Path) {
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { return @{ rc = 127; output = "TaidaFlowApp.exe not found: $Exe" } }
    $exeDir = Split-Path -Parent $Exe
    $savedPath = $env:PATH
    try {
        if (-not (Test-Path (Join-Path $exeDir 'Qt6Core.dll'))) {
            # Development build: the Qt DLLs come from the Qt installation (TAIDAFLOW_QT_BIN = its bin
            # folder, set by the desktop build; else TAIDAFLOW_QT_ROOT\msvc2022_64\bin; else C:\Qt\6.8.3).
            $qtBin = if ($env:TAIDAFLOW_QT_BIN) { $env:TAIDAFLOW_QT_BIN }
                     elseif ($env:TAIDAFLOW_QT_ROOT) { Join-Path $env:TAIDAFLOW_QT_ROOT 'msvc2022_64\bin' }
                     else { 'C:\Qt\6.8.3\msvc2022_64\bin' }
            $env:PATH = $qtBin + ';' + $env:PATH
        }
        $tmp = [System.IO.Path]::GetTempFileName()
        $tmpErr = [System.IO.Path]::GetTempFileName()
        # The path is quoted: it may contain spaces (-ArgumentList is one command line).
        $p = Start-Process -FilePath $Exe -ArgumentList @('--write-default-config', ('"' + $Path + '"')) -NoNewWindow -PassThru `
                 -RedirectStandardOutput $tmp -RedirectStandardError $tmpErr
        $null = $p.Handle
        if (-not $p.WaitForExit(60000)) { try { $p.Kill() } catch { }; return @{ rc = 124; output = "TaidaFlowApp.exe --write-default-config did not finish within 60 s" } }
        $out = (([System.IO.File]::ReadAllText($tmp) + [System.IO.File]::ReadAllText($tmpErr)) -split "`r?`n" | Where-Object { $_.Trim() -ne '' }) -join ' | '
        Remove-Item -LiteralPath $tmp, $tmpErr -Force -ErrorAction SilentlyContinue -WhatIf:$false
        return @{ rc = $p.ExitCode; output = $out }
    } finally { $env:PATH = $savedPath }
}

# Flattened default values (ordered key list + hashtable), from the exe; cached per exe.
function Get-TaidaFlowDefaultConfig([string]$Exe) {
    $full = [System.IO.Path]::GetFullPath($Exe)
    if ($script:TaidaFlowDefaultCache.ContainsKey($full)) { return $script:TaidaFlowDefaultCache[$full] }
    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ('taidaflow-default-' + [guid]::NewGuid().ToString('N'))
    $file = Join-Path $dir 'config.json'
    try {
        $r = Invoke-TaidaFlowWriteDefaultConfig $full $file
        if ($r.rc -ne 0) { throw "cannot get the default configuration from $full (exit $($r.rc): $($r.output))" }
        $obj = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    } finally { Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue -WhatIf:$false }
    $keys = New-Object System.Collections.Generic.List[string]
    $values = @{}
    function Add-Flat($o, [string]$prefix) {
        foreach ($p in $o.PSObject.Properties) {
            $k = if ($prefix) { "$prefix.$($p.Name)" } else { $p.Name }
            if ($p.Value -is [System.Management.Automation.PSCustomObject]) { Add-Flat $p.Value $k }
            else { $keys.Add($k); $values[$k] = $p.Value }
        }
    }
    Add-Flat $obj ''
    # w2-065: the scripts need these keys; they are app keys since w2-064 (no script fallback any more).
    $missing = @('nginx.exe', 'log.dir', 'log.quiet.keepDays', 'log.full.keepDays') | Where-Object { -not $values.ContainsKey($_) }
    if ($missing) { throw "the default configuration of $full has no $($missing -join ', ') - TaidaFlowApp.exe is older than these scripts (use the exe of the same package / build)" }
    $result = [pscustomobject]@{ Keys = $keys.ToArray(); Values = $values }
    $script:TaidaFlowDefaultCache[$full] = $result
    return $result
}

function Get-TaidaFlowConfig {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Exe, [switch]$Create)
    $Path = [System.IO.Path]::GetFullPath($Path)
    $cfg = [pscustomobject]@{
        Path = $Path; Exists = $false; Created = $false; Error = ''; Keys = @(); Values = @{}; Sources = @{}
        Notes = (New-Object System.Collections.Generic.List[string]); DataDir = ''; BaseDir = (Split-Path -Parent $Path); Exe = $Exe
    }
    if (Test-Path -LiteralPath $Path -PathType Container) { $cfg.Error = "$Path is a folder, not a file"; return $cfg }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -and $Create) {
        $w = Invoke-TaidaFlowWriteDefaultConfig $Exe $Path
        if ($w.rc -eq 0) { $cfg.Created = $true; $cfg.Notes.Add("config.json did not exist - created with the defaults by TaidaFlowApp.exe --write-default-config: $($w.output)") }
        elseif ($w.rc -ne 3) { $cfg.Error = "config.json $Path does not exist and could not be created (TaidaFlowApp.exe --write-default-config exit $($w.rc): $($w.output))"; return $cfg }
    }
    $defaults = $null
    try { $defaults = Get-TaidaFlowDefaultConfig $Exe } catch { $cfg.Error = $_.Exception.Message; return $cfg }
    $cfg.Keys = $defaults.Keys
    foreach ($k in $defaults.Keys) {
        $cfg.Values[$k] = $defaults.Values[$k]
        $cfg.Sources[$k] = 'default'
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $cfg.Notes.Add("config.json $Path does not exist - the default values are used (the application creates the file at its first start)")
    } else {
        $cfg.Exists = $true
        try {
            $text = [System.IO.File]::ReadAllText($Path, (New-Object System.Text.UTF8Encoding($false)))
            if ($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
            if ($text.Trim() -eq '') { throw 'the file is empty' }
            $root = $text | ConvertFrom-Json -ErrorAction Stop
        } catch {
            $cfg.Error = "config.json $Path is not valid JSON ($(($_.Exception.Message -split "`r?`n")[0])) - fix it or delete it to recreate the defaults (a Windows path needs \\ in JSON, e.g. `"C:\\TaidaFlowData`")"
            return $cfg
        }
        if ($root -isnot [System.Management.Automation.PSCustomObject]) { $cfg.Error = "config.json $Path : the top level is not a JSON object"; return $cfg }
        $badGroups = @{}
        foreach ($k in $defaults.Keys) {
            $segs = $k.Split('.')
            $o = $root; $missing = $false; $invalid = $false; $group = ''
            for ($i = 0; $i -lt $segs.Count - 1; $i++) {
                $group = if ($group) { "$group.$($segs[$i])" } else { $segs[$i] }
                $prop = $o.PSObject.Properties[$segs[$i]]
                if (-not $prop) { $missing = $true; break }
                if ($prop.Value -isnot [System.Management.Automation.PSCustomObject]) {
                    $invalid = $true
                    if (-not $badGroups.ContainsKey($group)) { $badGroups[$group] = 1; $cfg.Notes.Add("WARNING: `"$group`" is not an object - every setting in it uses its default") }
                    break
                }
                $o = $prop.Value
            }
            if ($invalid) { continue }
            $leaf = if ($missing) { $null } else { $o.PSObject.Properties[$segs[-1]] }
            if (-not $leaf) { $cfg.Notes.Add("`"$k`" is missing - using the default $($defaults.Values[$k])"); continue }
            $t = Test-TaidaFlowValue $k $leaf.Value
            if (-not $t.ok) { $cfg.Notes.Add("WARNING: `"$k`" = $($leaf.Value) is invalid ($($t.reason)) - using the default $($defaults.Values[$k])"); continue }
            $cfg.Values[$k] = $t.value; $cfg.Sources[$k] = 'file'
        }
        function Walk-Unknown($o, [string]$prefix) {
            foreach ($p in $o.PSObject.Properties) {
                $k = if ($prefix) { "$prefix.$($p.Name)" } else { $p.Name }
                if ($defaults.Values.ContainsKey($k)) { continue }
                if (@($defaults.Keys | Where-Object { $_.StartsWith("$k.") }).Count) {
                    if ($p.Value -is [System.Management.Automation.PSCustomObject]) { Walk-Unknown $p.Value $k }
                    continue
                }
                $cfg.Notes.Add("unknown key `"$k`" ignored")
            }
        }
        Walk-Unknown $root ''
    }
    Update-TaidaFlowDataDir $cfg
    return $cfg
}

function Update-TaidaFlowDataDir($cfg) {
    $d = [string]$cfg.Values['dataDir']
    try {
        if (-not [System.IO.Path]::IsPathRooted($d)) { $d = Join-Path $cfg.BaseDir $d }
        $cfg.DataDir = [System.IO.Path]::GetFullPath($d).TrimEnd('\')
    } catch { $cfg.Error = "dataDir `"$($cfg.Values['dataDir'])`" is not a usable path ($($_.Exception.Message))" }
}

function Set-TaidaFlowConfigValue($cfg, [string]$key, $value) {
    $t = Test-TaidaFlowValue $key $value
    if (-not $t.ok) { throw "override $key = $value is invalid ($($t.reason))" }
    $cfg.Values[$key] = $t.value
    $cfg.Sources[$key] = 'override'
    if ($key -eq 'dataDir') { Update-TaidaFlowDataDir $cfg }
}

function ConvertTo-TaidaFlowJsonValue($v) {
    if ($v -is [bool]) { if ($v) { return 'true' } else { return 'false' } }
    if ($v -is [string]) {
        $s = $v.Replace('\', '\\').Replace('"', '\"')
        $s = [regex]::Replace($s, '[\x00-\x1f]', { param($m) '\u{0:x4}' -f [int][char]$m.Value })
        return '"' + $s + '"'
    }
    return [string]([int64]$v)
}

# Complete config.json (same layout as the default file) with the effective values; dataDir is
# written as the resolved full path, so the file can live anywhere.
function Write-TaidaFlowConfigFile($cfg, [string]$path) {
    $sb = New-Object System.Text.StringBuilder
    $open = New-Object System.Collections.Generic.List[string]
    [void]$sb.Append("{`n")
    $keys = @($cfg.Keys)
    for ($i = 0; $i -lt $keys.Count; $i++) {
        $segs = $keys[$i].Split('.')
        $groups = $segs[0..($segs.Count - 2)]
        if ($segs.Count -eq 1) { $groups = @() }
        # close groups that end here
        $common = 0
        while ($common -lt $open.Count -and $common -lt $groups.Count -and $open[$common] -eq $groups[$common]) { $common++ }
        $first = $true
        while ($open.Count -gt $common) {
            $open.RemoveAt($open.Count - 1)
            [void]$sb.Append("`n" + ('  ' * ($open.Count + 1)) + '}')
            $first = $false
        }
        if ($i -gt 0) { [void]$sb.Append(",`n") }
        for ($g = $common; $g -lt $groups.Count; $g++) {
            [void]$sb.Append(('  ' * ($open.Count + 1)) + '"' + $groups[$g] + '": {' + "`n")
            $open.Add($groups[$g])
        }
        # Paths relative to the config folder are written as full paths (the file may live elsewhere).
        $value = if ($keys[$i] -eq 'dataDir') { $cfg.DataDir } elseif ($keys[$i] -eq 'nginx.exe') { Resolve-TaidaFlowNginxExe $cfg } else { $cfg.Values[$keys[$i]] }
        [void]$sb.Append(('  ' * ($open.Count + 1)) + '"' + $segs[-1] + '": ' + (ConvertTo-TaidaFlowJsonValue $value))
    }
    while ($open.Count -gt 0) { $open.RemoveAt($open.Count - 1); [void]$sb.Append("`n" + ('  ' * ($open.Count + 1)) + '}') }
    [void]$sb.Append("`n}`n")
    $dir = Split-Path -Parent $path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [System.IO.File]::WriteAllText($path, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
}

function Resolve-TaidaFlowNginxExe($cfg) {
    $p = [string]$cfg.Values['nginx.exe']
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $cfg.BaseDir $p }
    $full = [System.IO.Path]::GetFullPath($p)
    if (Test-Path -LiteralPath $full -PathType Container) { $full = Join-Path $full 'nginx.exe' }
    return $full
}

# log.dir resolved like AppConfig::resolvedLogDir(): relative to the resolved dataDir, absolute as it is.
function Resolve-TaidaFlowLogDir($cfg) {
    $p = [string]$cfg.Values['log.dir']
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $cfg.DataDir $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}

# Deletes the files <Prefix>YYYY-MM-DD<Suffix> directly in $Dir (no sub-folders, no links) whose date is
# older than $KeepDays days, today included - the retention rule of the app's own log files
# (App/applog.cpp: kept = date >= today - (keepDays - 1); dates after today are kept). The name must
# match completely (case-sensitive, ASCII digits, a valid date); every other file is left alone.
# Returns @{ Deleted = [string[]]; Failed = [string[]]; Kept = <n matching files kept> }.
function Remove-TaidaFlowExpiredDatedFiles([string]$Dir, [string]$Prefix, [string]$Suffix, [int]$KeepDays, [datetime]$Today = (Get-Date)) {
    $result = @{ Deleted = New-Object System.Collections.Generic.List[string]; Failed = New-Object System.Collections.Generic.List[string]; Kept = 0 }
    if ($KeepDays -lt 1 -or -not (Test-Path -LiteralPath $Dir -PathType Container)) { return $result }
    $re = New-Object System.Text.RegularExpressions.Regex ('^' + [regex]::Escape($Prefix) + '([0-9]{4})-([0-9]{2})-([0-9]{2})' + [regex]::Escape($Suffix) + '$')
    $oldest = $Today.Date.AddDays(-($KeepDays - 1))
    foreach ($f in @(Get-ChildItem -LiteralPath $Dir -File -Force -ErrorAction SilentlyContinue)) {
        if ($f.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { continue }
        $m = $re.Match($f.Name)
        if (-not $m.Success) { continue }
        $date = [datetime]::MinValue
        if (-not [datetime]::TryParseExact("$($m.Groups[1].Value)-$($m.Groups[2].Value)-$($m.Groups[3].Value)", 'yyyy-MM-dd',
                [System.Globalization.CultureInfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::None, [ref]$date)) { continue }
        if ($date -ge $oldest) { $result.Kept++; continue }
        try { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop -WhatIf:$false; $result.Deleted.Add($f.Name) }
        catch { $result.Failed.Add("$($f.Name): $($_.Exception.Message)") }
    }
    return $result
}

# Path of $to relative to the folder $from ("..\web"), or $to itself when they are on different drives.
function Get-TaidaFlowRelativePath([string]$from, [string]$to) {
    $a = [System.IO.Path]::GetFullPath($from).TrimEnd('\').Split('\')
    $b = [System.IO.Path]::GetFullPath($to).TrimEnd('\').Split('\')
    if (-not [string]::Equals($a[0], $b[0], [System.StringComparison]::OrdinalIgnoreCase)) { return [System.IO.Path]::GetFullPath($to) }
    $i = 0
    while ($i -lt $a.Count -and $i -lt $b.Count -and [string]::Equals($a[$i], $b[$i], [System.StringComparison]::OrdinalIgnoreCase)) { $i++ }
    $parts = @()
    for ($k = $i; $k -lt $a.Count; $k++) { $parts += '..' }
    for ($k = $i; $k -lt $b.Count; $k++) { $parts += $b[$k] }
    if ($parts.Count -eq 0) { return '.' }
    return ($parts -join '\')
}

# The complete TaidaFlow nginx configuration: the template deploy\nginx\taidaflow.conf with its six
# @TOKENS@ replaced. $values: WebRoot, ExportDir, LogDir (folders, written with forward slashes), Port,
# RestPort, MirrorPort, optional Prefix (the nginx folder = prefix of "start nginx"): then the web
# root is written relative to it (Mango A9: nginx resolves a relative "root" against its prefix, so
# moving the whole installation folder keeps the page working). LogDir (w2-065) = log.dir resolved
# (absolute): nginx-error.log and nginx-access-YYYY-MM-DD.log. Throws when a token is missing.
function Get-TaidaFlowNginxConf([string]$template, $values) {
    $text = [System.IO.File]::ReadAllText($template)
    $webRoot = if ($values.Prefix) { Get-TaidaFlowRelativePath $values.Prefix $values.WebRoot } else { $values.WebRoot }
    if (-not $values.LogDir) { throw "Get-TaidaFlowNginxConf: LogDir missing" }
    $map = [ordered]@{
        '@TAIDAFLOW_WEB_ROOT@' = ($webRoot -replace '\\', '/'); '@TAIDAFLOW_EXPORT_DIR@' = ($values.ExportDir -replace '\\', '/')
        '@TAIDAFLOW_LOG_DIR@' = ($values.LogDir -replace '\\', '/')
        '@TAIDAFLOW_NGINX_PORT@' = "$($values.Port)"; '@TAIDAFLOW_REST_PORT@' = "$($values.RestPort)"; '@TAIDAFLOW_MIRROR_PORT@' = "$($values.MirrorPort)"
    }
    foreach ($k in $map.Keys) {
        if (-not $text.Contains($k)) { throw "template has no ${k}: $template" }
        $text = $text.Replace($k, $map[$k])
    }
    return $text
}

function Get-TaidaFlowDownloadPort($cfg) {
    if ($cfg.Values['nginx.enabled']) { return [int]$cfg.Values['nginx.port'] }
    return [int]$cfg.Values['http.port']
}

# Mirror port written to runtime.json (Mango A2, same rule as Core::startHttpServer): with nginx the
# page connects through nginx (location /mirror), so nginx.port; without nginx mirror.publicPort.
function Get-TaidaFlowPagePort($cfg) {
    if ($cfg.Values['nginx.enabled']) { return [int]$cfg.Values['nginx.port'] }
    return [int]$cfg.Values['mirror.publicPort']
}

function Get-TaidaFlowRuntimeJson([int]$port) { return '{"mirrorPublicPort":' + $port + ',"version":1}' }

function Write-TaidaFlowRuntimeJson([string]$webDir, [int]$port) {
    $path = Join-Path $webDir 'runtime.json'
    $body = Get-TaidaFlowRuntimeJson $port
    $gz = "$path.gz"
    if (Test-Path -LiteralPath $gz) { Remove-Item -LiteralPath $gz -Force }
    if ((Test-Path -LiteralPath $path -PathType Leaf) -and [System.IO.File]::ReadAllText($path) -ceq $body) { return "runtime.json unchanged: $path = $body" }
    [System.IO.File]::WriteAllText($path, $body, (New-Object System.Text.UTF8Encoding($false)))
    return "runtime.json written: $path = $body"
}

# One line per value for logs: "key = value (source)".
function Format-TaidaFlowConfig($cfg) {
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($k in $cfg.Keys) { $lines.Add(("  {0} = {1} ({2})" -f $k, (ConvertTo-TaidaFlowJsonValue $cfg.Values[$k]), $cfg.Sources[$k])) }
    $lines.Add("  dataDir (resolved) = $($cfg.DataDir)")
    $lines.Add("  nginx.exe (resolved) = $(Resolve-TaidaFlowNginxExe $cfg)")
    $lines.Add("  log.dir (resolved) = $(Resolve-TaidaFlowLogDir $cfg)")
    return $lines.ToArray()
}

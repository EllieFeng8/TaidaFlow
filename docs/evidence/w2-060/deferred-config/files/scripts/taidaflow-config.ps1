# TaidaFlow site configuration reader (w2-060) - dot-sourced by the field scripts
# (start-taidaflow.ps1, stop-taidaflow.ps1, register-autostart.ps1) and by scripts\nginx-web.ps1.
#
# The ONLY site configuration file is config.json next to TaidaFlowApp.exe (the package folder):
#   {
#     "dataDir":   "C:\\TaidaFlowData",   data folder = working directory of the app (string; relative
#                                         = to the folder of config.json). Read by the APP itself
#                                         (Core, before any file is written) and by these scripts.
#     "useNginx":  true,                  start nginx with the app (bool)            - scripts only
#     "nginxPort": 80,                    nginx listen port, 1..65535 (int)           - scripts only
#     "restPort":  18080                  internal REST port on 127.0.0.1 (int)       - scripts only
#   }
# Keys starting with "_" are comments (ignored). Any other key is reported and ignored.
# JSON needs "\\" for one backslash ("C:\\TaidaFlowData"); "C:/TaidaFlowData" also works.
#
# Read-TaidaFlowConfig <folder> returns an object:
#   Path, Found (file exists), Error ('' or why the file / a value is unusable - the caller refuses to
#   start), DataDir (full path or ''), UseNginx ($true/$false/$null), NginxPort / RestPort (int or 0),
#   Notes (list of messages for the log: ignored keys ...).
# Values that are missing stay empty ($null / 0 / ''); the caller applies its own default.

function Read-TaidaFlowConfig([string]$folder) {
    $path = Join-Path $folder 'config.json'
    $r = [pscustomobject]@{ Path = $path; Found = $false; Error = ''; DataDir = ''; UseNginx = $null
                            NginxPort = 0; RestPort = 0; Notes = (New-Object System.Collections.Generic.List[string]) }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $r }
    $r.Found = $true
    try {
        # ReadAllText: UTF-8 (a BOM is accepted); Windows PowerShell's Get-Content would use the ANSI code page.
        $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8).TrimStart([char]0xFEFF)
        $obj = $text | ConvertFrom-Json -ErrorAction Stop
    } catch {
        $r.Error = "$path is not valid JSON ($($_.Exception.Message.Split([Environment]::NewLine)[0])); a Windows path needs \\ in JSON, e.g. `"C:\\TaidaFlowData`""
        return $r
    }
    if ($null -eq $obj -or $obj -isnot [System.Management.Automation.PSCustomObject]) {
        $r.Error = "$path must hold one JSON object { ... }"
        return $r
    }
    foreach ($p in $obj.PSObject.Properties) {
        $name = $p.Name; $v = $p.Value
        switch -CaseSensitive ($name) {
            'dataDir' {
                if ($v -is [string] -and $v.Trim() -ne '') {
                    $d = $v.Trim()
                    try {
                        if (-not [System.IO.Path]::IsPathRooted($d)) { $d = Join-Path $folder $d }
                        $r.DataDir = [System.IO.Path]::GetFullPath($d).TrimEnd('\')
                    } catch { $r.Error = "$path : dataDir `"$v`" is not a usable path ($($_.Exception.Message))" }
                } else { $r.Error = "$path : dataDir must be a non-empty string" }
            }
            'useNginx' {
                if ($v -is [bool]) { $r.UseNginx = $v } else { $r.Error = "$path : useNginx must be true or false" }
            }
            'nginxPort' {
                if (($v -is [int] -or $v -is [long]) -and $v -ge 1 -and $v -le 65535) { $r.NginxPort = [int]$v }
                else { $r.Error = "$path : nginxPort must be a whole number 1..65535" }
            }
            'restPort' {
                if (($v -is [int] -or $v -is [long]) -and $v -ge 1 -and $v -le 65535) { $r.RestPort = [int]$v }
                else { $r.Error = "$path : restPort must be a whole number 1..65535" }
            }
            default {
                if (-not $name.StartsWith('_')) { $r.Notes.Add("$path : unsupported key `"$name`" ignored") }
            }
        }
    }
    return $r
}

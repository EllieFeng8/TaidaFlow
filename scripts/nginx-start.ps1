# Start the nginx web front end (w2-050, w2-062) = scripts\nginx-web.ps1 -Action start.
# Everything comes from config.json (w2-062): listen port nginx.port, /api/ -> 127.0.0.1:rest.port,
# /mirror -> 127.0.0.1:mirror.internalPort, downloads from <dataDir>\exports, the program nginx.exe.
# Config file: -Config, else TAIDAFLOW_CONFIG, else deploy\dev\config.dev.json (repository).
# Deploy the page first:  scripts\deploy-web.ps1   (build\wasm-release -> <exe folder>\web + .gz)
# Usage: powershell -ExecutionPolicy Bypass -File scripts\nginx-start.ps1 [-Config <config.json>]
#            [-WebRoot <folder>] [-ExeDir <folder>] [-ExportDir <folder>] [-RuntimeDir <folder>]
#            [-Nginx <nginx.exe or folder>] [-Port <n>] [-RestPort <n>] [-MirrorPort <n>] [-Test]
#   -Test: only render the configuration and run nginx -t.
#   -Port / -RestPort / -MirrorPort / -Nginx / -ExportDir: one-time overrides of config.json.
# Exit codes: see scripts\nginx-web.ps1 (0 started, 2 missing, 3 nginx -t failed, 4 port busy (owner
# listed), 5 link/junction in the web root or export folder, 6 not listening in time).
param(
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
    [int]$TimeoutSec = 20,
    [switch]$Test
)
$forward = @{} + $PSBoundParameters
$forward.Remove('Test') | Out-Null
$action = if ($Test) { 'test' } else { 'start' }
& (Join-Path $PSScriptRoot 'nginx-web.ps1') -Action $action @forward
exit $LASTEXITCODE
